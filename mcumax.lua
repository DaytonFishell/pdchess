-- mcu-max
-- Lua 5.3+ chess engine based on the public behavior of the C implementation.

local M = {
    ID = "mcu-max Lua 1.0.0",
    AUTHOR = "Gissio; Lua port",

    EMPTY = 0,
    PAWN_UPSTREAM = 1,
    PAWN_DOWNSTREAM = 2,
    KNIGHT = 3,
    KING = 4,
    BISHOP = 5,
    ROOK = 6,
    QUEEN = 7,
    BLACK = 0x8,
    SQUARE_INVALID = 0x80,
}

local WHITE, BLACK = 1, -1
local PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING = 1, 2, 3, 4, 5, 6
local PIECE_VALUE = {100, 320, 330, 500, 900, 0}
local KNIGHT_STEPS = {-33, -31, -18, -14, 14, 18, 31, 33}
local BISHOP_STEPS = {-17, -15, 15, 17}
local ROOK_STEPS = {-16, -1, 1, 16}
local KING_STEPS = {-17, -16, -15, -1, 1, 15, 16, 17}
local BACK_RANK = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK}
local INF, MATE = 1000000000, 1000000

local Engine = {}
Engine.__index = Engine

local function on_board(square)
    return square >= 0 and square < 128 and (square & 0x88) == 0
end

local function color_of(piece)
    if piece > 0 then return WHITE end
    if piece < 0 then return BLACK end
    return 0
end

local function type_of(piece)
    return math.abs(piece)
end

local function copy_move(move)
    if not move then return nil end
    return {from = move.from, to = move.to, promotion = move.promotion}
end

local function add_move(moves, from, to, promotion, special)
    moves[#moves + 1] = {
        from = from,
        to = to,
        promotion = promotion,
        special = special,
    }
end

function M.parse_square(text)
    if type(text) ~= "string" or #text < 2 then return nil end
    local file = text:byte(1) - string.byte("a")
    local rank = string.byte("8") - text:byte(2)
    local square = rank * 16 + file
    if file < 0 or file > 7 or rank < 0 or rank > 7 then return nil end
    return square
end

function M.format_square(square)
    if not on_board(square) then return nil end
    return string.char(string.byte("a") + (square & 7))
        .. string.char(string.byte("8") - (square >> 4))
end

function M.parse_move(text)
    if type(text) ~= "string" or (#text ~= 4 and #text ~= 5) then return nil end
    if #text == 5 and text:sub(5, 5):lower() ~= "q" then return nil end
    local from = M.parse_square(text:sub(1, 2))
    local to = M.parse_square(text:sub(3, 4))
    if not from or not to then return nil end
    return {from = from, to = to, promotion = #text == 5 and QUEEN or nil}
end

function M.format_move(move)
    if not move or move.from == M.SQUARE_INVALID or move.to == M.SQUARE_INVALID then
        return "0000"
    end
    return M.format_square(move.from) .. M.format_square(move.to)
        .. (move.promotion and "q" or "")
end

function Engine:_clear()
    self.board = {}
    for square = 0, 127 do self.board[square] = 0 end
    self.side = WHITE
    self.castling = {K = false, Q = false, k = false, q = false}
    self.en_passant = M.SQUARE_INVALID
    self.halfmove = 0
    self.fullmove = 1
    self.node_count = 0
    self.node_max = 0
    self.stop_search = false
    self.callback_interval = 1024
end

function Engine:init()
    self:_clear()
    for file = 0, 7 do
        self.board[file] = -BACK_RANK[file + 1]
        self.board[16 + file] = -PAWN
        self.board[96 + file] = PAWN
        self.board[112 + file] = BACK_RANK[file + 1]
    end
    self.castling.K, self.castling.Q = true, true
    self.castling.k, self.castling.q = true, true
    return self
end

function M.new()
    return setmetatable({}, Engine):init()
end

function Engine:clone()
    local copy = M.new()
    for square = 0, 127 do copy.board[square] = self.board[square] end
    copy.side = self.side
    copy.castling = {
        K = self.castling.K,
        Q = self.castling.Q,
        k = self.castling.k,
        q = self.castling.q,
    }
    copy.en_passant = self.en_passant
    copy.halfmove = self.halfmove
    copy.fullmove = self.fullmove
    return copy
end

function Engine:set_fen_position(fen)
    assert(type(fen) == "string", "FEN must be a string")
    local fields = {}
    for field in fen:gmatch("%S+") do fields[#fields + 1] = field end
    assert(fields[1] and fields[2], "invalid FEN")

    self:_clear()
    local rank = 0
    for row in fields[1]:gmatch("[^/]+") do
        assert(rank < 8, "invalid FEN board")
        local file = 0
        for char in row:gmatch(".") do
            local empty = tonumber(char)
            if empty then
                file = file + empty
            else
                local lower = char:lower()
                local types = {p = PAWN, n = KNIGHT, b = BISHOP, r = ROOK, q = QUEEN, k = KING}
                local piece_type = types[lower]
                assert(piece_type and file < 8, "invalid FEN piece")
                self.board[rank * 16 + file] =
                    char == lower and -piece_type or piece_type
                file = file + 1
            end
        end
        assert(file == 8, "invalid FEN rank")
        rank = rank + 1
    end
    assert(rank == 8, "invalid FEN board")

    self.side = fields[2] == "b" and BLACK or WHITE
    local rights = fields[3] or "-"
    for key in pairs(self.castling) do
        self.castling[key] = rights:find(key, 1, true) ~= nil
    end
    self.en_passant = fields[4] and fields[4] ~= "-"
        and (M.parse_square(fields[4]) or M.SQUARE_INVALID)
        or M.SQUARE_INVALID
    self.halfmove = tonumber(fields[5]) or 0
    self.fullmove = tonumber(fields[6]) or 1
    return self
end

function Engine:get_piece(square)
    if not on_board(square) then return M.EMPTY end
    local piece = self.board[square]
    if piece == 0 then return M.EMPTY end
    local public_types = {
        [PAWN] = piece > 0 and M.PAWN_UPSTREAM or M.PAWN_DOWNSTREAM,
        [KNIGHT] = M.KNIGHT,
        [BISHOP] = M.BISHOP,
        [ROOK] = M.ROOK,
        [QUEEN] = M.QUEEN,
        [KING] = M.KING,
    }
    return public_types[type_of(piece)] | (piece < 0 and M.BLACK or 0)
end

function Engine:get_current_side()
    return self.side == BLACK and M.BLACK or M.EMPTY
end

function Engine:_find_king(side)
    for square = 0, 127 do
        if on_board(square) and self.board[square] == side * KING then
            return square
        end
    end
    return M.SQUARE_INVALID
end

function Engine:_is_attacked(square, by_side)
    local pawn_source = by_side == WHITE and 16 or -16
    for _, delta in ipairs({pawn_source - 1, pawn_source + 1}) do
        local source = square + delta
        if on_board(source) and self.board[source] == by_side * PAWN then return true end
    end
    for _, delta in ipairs(KNIGHT_STEPS) do
        local source = square + delta
        if on_board(source) and self.board[source] == by_side * KNIGHT then return true end
    end
    for _, delta in ipairs(KING_STEPS) do
        local source = square + delta
        if on_board(source) and self.board[source] == by_side * KING then return true end
    end
    for _, ray in ipairs({
        {steps = BISHOP_STEPS, first = BISHOP, second = QUEEN},
        {steps = ROOK_STEPS, first = ROOK, second = QUEEN},
    }) do
        for _, delta in ipairs(ray.steps) do
            local source = square + delta
            while on_board(source) do
                local piece = self.board[source]
                if piece ~= 0 then
                    local kind = type_of(piece)
                    if color_of(piece) == by_side and
                        (kind == ray.first or kind == ray.second) then return true end
                    break
                end
                source = source + delta
            end
        end
    end
    return false
end

function Engine:_in_check(side)
    local king = self:_find_king(side)
    return king == M.SQUARE_INVALID or self:_is_attacked(king, -side)
end

function Engine:_pseudo_moves(captures_only)
    local moves = {}
    for from = 0, 127 do
        local piece = on_board(from) and self.board[from] or 0
        if color_of(piece) == self.side then
            local kind = type_of(piece)
            if kind == PAWN then
                local step = self.side == WHITE and -16 or 16
                local promotion_rank = self.side == WHITE and 0 or 7
                local home_rank = self.side == WHITE and 6 or 1
                local one = from + step
                if not captures_only and on_board(one) and self.board[one] == 0 then
                    add_move(moves, from, one, (one >> 4) == promotion_rank and QUEEN or nil)
                    local two = one + step
                    if (from >> 4) == home_rank and self.board[two] == 0 then
                        add_move(moves, from, two, nil, "double")
                    end
                end
                for _, offset in ipairs({step - 1, step + 1}) do
                    local to = from + offset
                    if on_board(to) and
                        (color_of(self.board[to]) == -self.side or to == self.en_passant) then
                        add_move(moves, from, to,
                            (to >> 4) == promotion_rank and QUEEN or nil,
                            to == self.en_passant and "en_passant" or nil)
                    end
                end
            elseif kind == KNIGHT or kind == KING then
                local steps = kind == KNIGHT and KNIGHT_STEPS or KING_STEPS
                for _, delta in ipairs(steps) do
                    local to = from + delta
                    if on_board(to) and color_of(self.board[to]) ~= self.side and
                        (not captures_only or self.board[to] ~= 0) then
                        add_move(moves, from, to)
                    end
                end
                if kind == KING and not captures_only and not self:_in_check(self.side) then
                    local rank_base = self.side == WHITE and 112 or 0
                    local king_right = self.side == WHITE and "K" or "k"
                    local queen_right = self.side == WHITE and "Q" or "q"
                    if from == rank_base + 4 and self.castling[king_right] and
                        self.board[rank_base + 5] == 0 and self.board[rank_base + 6] == 0 and
                        self.board[rank_base + 7] == self.side * ROOK and
                        not self:_is_attacked(rank_base + 5, -self.side) and
                        not self:_is_attacked(rank_base + 6, -self.side) then
                        add_move(moves, from, rank_base + 6, nil, "castle_k")
                    end
                    if from == rank_base + 4 and self.castling[queen_right] and
                        self.board[rank_base + 1] == 0 and self.board[rank_base + 2] == 0 and
                        self.board[rank_base + 3] == 0 and
                        self.board[rank_base] == self.side * ROOK and
                        not self:_is_attacked(rank_base + 3, -self.side) and
                        not self:_is_attacked(rank_base + 2, -self.side) then
                        add_move(moves, from, rank_base + 2, nil, "castle_q")
                    end
                end
            else
                local steps = kind == BISHOP and BISHOP_STEPS
                    or kind == ROOK and ROOK_STEPS
                    or KING_STEPS
                for _, delta in ipairs(steps) do
                    local to = from + delta
                    while on_board(to) do
                        local target = self.board[to]
                        if target == 0 then
                            if not captures_only then add_move(moves, from, to) end
                        else
                            if color_of(target) == -self.side then add_move(moves, from, to) end
                            break
                        end
                        to = to + delta
                    end
                end
            end
        end
    end
    return moves
end

function Engine:_make(move)
    local undo = {
        captured = self.board[move.to],
        piece = self.board[move.from],
        en_passant = self.en_passant,
        castling = {
            K = self.castling.K, Q = self.castling.Q,
            k = self.castling.k, q = self.castling.q,
        },
        halfmove = self.halfmove,
        fullmove = self.fullmove,
        special = move.special,
    }
    local piece = undo.piece
    self.board[move.from] = 0
    self.board[move.to] = piece

    if move.special == "en_passant" then
        undo.capture_square = move.to + (self.side == WHITE and 16 or -16)
        undo.captured = self.board[undo.capture_square]
        self.board[undo.capture_square] = 0
    elseif move.special == "castle_k" then
        undo.rook_from, undo.rook_to = move.from + 3, move.from + 1
        self.board[undo.rook_to], self.board[undo.rook_from] =
            self.board[undo.rook_from], 0
    elseif move.special == "castle_q" then
        undo.rook_from, undo.rook_to = move.from - 4, move.from - 1
        self.board[undo.rook_to], self.board[undo.rook_from] =
            self.board[undo.rook_from], 0
    end

    if move.promotion then self.board[move.to] = self.side * QUEEN end
    self.en_passant = move.special == "double"
        and (move.from + move.to) // 2 or M.SQUARE_INVALID

    if type_of(piece) == KING then
        if self.side == WHITE then self.castling.K, self.castling.Q = false, false
        else self.castling.k, self.castling.q = false, false end
    end
    local rights_by_square = {
        [112] = "Q", [119] = "K", [0] = "q", [7] = "k",
    }
    local from_right, to_right = rights_by_square[move.from], rights_by_square[move.to]
    if from_right then self.castling[from_right] = false end
    if to_right then self.castling[to_right] = false end

    if type_of(piece) == PAWN or undo.captured ~= 0 then self.halfmove = 0
    else self.halfmove = self.halfmove + 1 end
    if self.side == BLACK then self.fullmove = self.fullmove + 1 end
    self.side = -self.side
    return undo
end

function Engine:_unmake(move, undo)
    self.side = -self.side
    self.board[move.from] = undo.piece
    self.board[move.to] = undo.captured
    if undo.capture_square then
        self.board[move.to] = 0
        self.board[undo.capture_square] = undo.captured
    end
    if undo.rook_from then
        self.board[undo.rook_from], self.board[undo.rook_to] =
            self.board[undo.rook_to], 0
    end
    self.en_passant = undo.en_passant
    self.castling = undo.castling
    self.halfmove, self.fullmove = undo.halfmove, undo.fullmove
end

function Engine:_legal_moves(captures_only)
    local side = self.side
    local legal = {}
    for _, move in ipairs(self:_pseudo_moves(captures_only)) do
        local undo = self:_make(move)
        local valid = not self:_in_check(side)
        self:_unmake(move, undo)
        if valid then legal[#legal + 1] = move end
    end
    return legal
end

function Engine:search_valid_moves()
    local result = {}
    for _, move in ipairs(self:_legal_moves(false)) do
        result[#result + 1] = copy_move(move)
    end
    return result
end

function Engine:play_move(requested)
    if not requested then return false end
    for _, move in ipairs(self:_legal_moves(false)) do
        if move.from == requested.from and move.to == requested.to and
            (not requested.promotion or requested.promotion == QUEEN) then
            self:_make(move)
            return true
        end
    end
    return false
end

function Engine:_evaluate()
    local score = 0
    for square = 0, 127 do
        local piece = on_board(square) and self.board[square] or 0
        if piece ~= 0 then
            local kind = type_of(piece)
            local value = PIECE_VALUE[kind]
            local file, rank = square & 7, square >> 4
            local center = 6 - math.abs(file * 2 - 7) - math.abs(rank * 2 - 7)
            score = score + color_of(piece) * (value + center)
        end
    end
    return score * self.side
end

function Engine:_tick()
    self.node_count = self.node_count + 1
    if self.callback and self.node_count % self.callback_interval == 0 then
        self.callback(self.callback_userdata)
    end
    if self.stop_search then return false end
    return self.node_max == 0 or self.node_count < self.node_max
end

function Engine:_quiescence(alpha, beta)
    if not self:_tick() then return self:_evaluate() end
    local stand = self:_evaluate()
    if stand >= beta then return beta end
    if stand > alpha then alpha = stand end
    for _, move in ipairs(self:_legal_moves(true)) do
        local undo = self:_make(move)
        local score = -self:_quiescence(-beta, -alpha)
        self:_unmake(move, undo)
        if self.stop_search then return alpha end
        if score >= beta then return beta end
        if score > alpha then alpha = score end
    end
    return alpha
end

function Engine:_negamax(depth, alpha, beta, ply)
    if depth <= 0 then
        if self:_in_check(self.side) then depth = 1
        else return self:_quiescence(alpha, beta) end
    end
    if not self:_tick() then return self:_evaluate() end
    local moves = self:_legal_moves(false)
    if #moves == 0 then
        return self:_in_check(self.side) and (-MATE + ply) or 0
    end
    table.sort(moves, function(a, b)
        return math.abs(self.board[a.to] or 0) > math.abs(self.board[b.to] or 0)
    end)
    local best = -INF
    for _, move in ipairs(moves) do
        local undo = self:_make(move)
        local score = -self:_negamax(depth - 1, -beta, -alpha, ply + 1)
        self:_unmake(move, undo)
        if self.stop_search then return best == -INF and alpha or best end
        if score > best then best = score end
        if score > alpha then alpha = score end
        if alpha >= beta then break end
    end
    return best
end

function Engine:search_best_move(node_max, depth_max)
    self.node_max = math.max(1, tonumber(node_max) or 100000)
    depth_max = math.max(1, math.min(tonumber(depth_max) or 6, 99))
    self.node_count, self.stop_search = 0, false
    local root_moves = self:_legal_moves(false)
    if #root_moves == 0 then return nil end
    local completed_best = root_moves[1]
    local completed_score = 0
    local completed_depth = 0

    for depth = 1, depth_max do
        local best, best_score = completed_best, -INF
        for _, move in ipairs(root_moves) do
            local undo = self:_make(move)
            local score = -self:_negamax(depth - 1, -INF, INF, 1)
            self:_unmake(move, undo)
            if self.stop_search or self.node_count >= self.node_max then break end
            if score > best_score then best, best_score = move, score end
        end
        if self.stop_search or self.node_count >= self.node_max then break end
        completed_best = best
        completed_score = best_score
        completed_depth = depth
        for index, move in ipairs(root_moves) do
            if move == completed_best then
                root_moves[1], root_moves[index] = root_moves[index], root_moves[1]
                break
            end
        end
    end
    self.search_info = {
        nodes = self.node_count,
        depth = completed_depth,
        score = completed_score,
    }
    return copy_move(completed_best)
end

function Engine:get_search_info()
    return self.search_info or {nodes = 0, depth = 0, score = 0}
end

function Engine:begin_search(node_max, depth_max, nodes_per_slice)
    local worker = self:clone()
    local task = {
        worker = worker,
        done = false,
        best_move = nil,
        info = {nodes = 0, depth = 0, score = 0},
    }
    worker:set_callback(function()
        coroutine.yield()
    end, nil, nodes_per_slice or 32)
    task.thread = coroutine.create(function()
        local best = worker:search_best_move(node_max, depth_max)
        return best, worker:get_search_info()
    end)
    return task
end

function Engine:step_search(task)
    assert(task and task.thread, "invalid search task")
    if task.done then return true, copy_move(task.best_move), task.info end

    local ok, best, info = coroutine.resume(task.thread)
    if not ok then error(best, 0) end

    task.info = {
        nodes = task.worker.node_count,
        depth = task.worker:get_search_info().depth,
        score = task.worker:get_search_info().score,
    }
    if coroutine.status(task.thread) == "dead" then
        task.done = true
        task.best_move = copy_move(best)
        task.info = info or task.info
    end
    return task.done, copy_move(task.best_move), task.info
end

function Engine:get_status()
    local moves = self:_legal_moves(false)
    if #moves == 0 then
        return self:_in_check(self.side) and "Checkmate" or "Stalemate"
    end
    return self:_in_check(self.side) and "Check" or "Playing"
end

function Engine:set_callback(callback, userdata, interval)
    self.callback, self.callback_userdata = callback, userdata
    self.callback_interval = math.max(1, tonumber(interval) or 1024)
end

function Engine:stop()
    self.stop_search = true
end

M.Engine = Engine
_G.mcumax = M
return M

local script = arg[0]:gsub("\\", "/")
local root = script:match("^(.*)/tests/test_mcumax%.lua$") or "."
package.path = root .. "/?.lua;" .. package.path

local mcumax = require("mcumax")

local function perft(engine, depth)
    if depth == 0 then return 1 end
    local nodes = 0
    for _, move in ipairs(engine:_legal_moves(false)) do
        local undo = engine:_make(move)
        nodes = nodes + perft(engine, depth - 1)
        engine:_unmake(move, undo)
    end
    return nodes
end

local function contains_move(engine, text)
    for _, move in ipairs(engine:search_valid_moves()) do
        if mcumax.format_move(move) == text then return true end
    end
    return false
end

local engine = mcumax.new()
assert(#engine:search_valid_moves() == 20, "start position must have 20 moves")
assert(perft(engine, 1) == 20, "perft depth 1")
assert(perft(engine, 2) == 400, "perft depth 2")
assert(perft(engine, 3) == 8902, "perft depth 3")

assert(engine:play_move(mcumax.parse_move("e2e4")), "e2e4 must be legal")
assert(engine:get_current_side() == mcumax.BLACK, "black must move after e2e4")

engine:set_fen_position("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")
assert(contains_move(engine, "e1g1"), "white king-side castling")
assert(contains_move(engine, "e1c1"), "white queen-side castling")

engine:set_fen_position("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1")
assert(contains_move(engine, "e5d6"), "en passant")
assert(engine:play_move(mcumax.parse_move("e5d6")), "apply en passant")
assert(engine:get_piece(mcumax.parse_square("d5")) == mcumax.EMPTY,
    "captured en passant pawn must be removed")

engine:set_fen_position("7k/P7/8/8/8/8/8/7K w - - 0 1")
assert(contains_move(engine, "a7a8q"), "queen promotion")
assert(engine:play_move(mcumax.parse_move("a7a8q")), "apply promotion")
assert((engine:get_piece(mcumax.parse_square("a8")) & 7) == mcumax.QUEEN,
    "promoted piece must be a queen")

engine:set_fen_position("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1")
assert(engine:get_status() == "Checkmate", "checkmate detection")

engine:set_fen_position("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
assert(engine:get_status() == "Stalemate", "stalemate detection")

engine:init()
local best = engine:search_best_move(2500, 3)
local info = engine:get_search_info()
assert(best and engine:play_move(best), "search must return a legal move")
assert(info.nodes > 0, "search must visit nodes")

engine:init()
local before = {}
for square = 0, 127 do before[square] = engine.board[square] end
local task = engine:begin_search(2500, 3, 8)
local done, async_best, async_info
local slices = 0
repeat
    done, async_best, async_info = engine:step_search(task)
    slices = slices + 1
    for square = 0, 127 do
        assert(engine.board[square] == before[square],
            "incremental search must not mutate the live position")
    end
    assert(slices < 10000, "incremental search did not finish")
until done
assert(slices > 1, "incremental search must yield between slices")
assert(async_best and engine:play_move(async_best),
    "incremental search must return a legal move")
assert(async_info.nodes > 0, "incremental search must visit nodes")

print("pdchess Lua tests passed")

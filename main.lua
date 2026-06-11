import "CoreLibs/graphics"
import "CoreLibs/ui"
import "mcumax"

local gfx = playdate.graphics

local BOARD_SIZE = 240
local CELL_SIZE = 30
local PANEL_X = 248

local positions = {
    { name = "Start" },
    { name = "Castling",   fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1" },
    { name = "En passant", fen = "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1" },
    { name = "Promotion",  fen = "7k/P7/8/8/8/8/8/7K w - - 0 1" },
    { name = "Mate",       fen = "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1" },
    { name = "Stalemate",  fen = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1" },
}

local difficulty_names = { "Easy", "Medium", "Hard" }
local node_limits = { 2500, 15000, 75000 }
local depth_limits = { 3, 5, 7 }
local piece_letters = {
    [mcumax.PAWN_UPSTREAM] = "P",
    [mcumax.PAWN_DOWNSTREAM] = "P",
    [mcumax.KNIGHT] = "N",
    [mcumax.BISHOP] = "B",
    [mcumax.ROOK] = "R",
    [mcumax.QUEEN] = "Q",
    [mcumax.KING] = "K",
}

local engine = mcumax.new()
local screen = "setup"
local human_black = false
local difficulty = 2
local position_index = 1
local cursor = mcumax.parse_square("e2")
local selected
local legal_moves = {}
local last_move
local ai_pending = false
local ai_thinking = false
local search_info
local crank_accumulator = 0
local crank_index = 0

local function is_black_piece(piece)
    return (piece & mcumax.BLACK) ~= 0
end

local function human_side()
    return human_black and mcumax.BLACK or mcumax.EMPTY
end

local function refresh_legal_moves()
    legal_moves = engine:search_valid_moves()
end

local function load_position(index)
    position_index = index
    local position = positions[index]
    if position.fen then engine:set_fen_position(position.fen) else engine:init() end
    selected = nil
    last_move = nil
    search_info = nil
    cursor = mcumax.parse_square(human_black and "e7" or "e2")
    refresh_legal_moves()
    ai_pending = not position.fen and engine:get_current_side() ~= human_side()
end

local function square_to_screen(square)
    local file = square & 7
    local rank = square >> 4
    local column = human_black and 7 - file or file
    local row = human_black and 7 - rank or rank
    return column * CELL_SIZE, row * CELL_SIZE
end

local function screen_to_square(column, row)
    local file = human_black and 7 - column or column
    local rank = human_black and 7 - row or row
    return rank * 16 + file
end

local function is_legal_target(square)
    if not selected then return false end
    for _, move in ipairs(legal_moves) do
        if move.from == selected and move.to == square then return true end
    end
    return false
end

local function draw_piece(square, piece)
    local x, y = square_to_screen(square)
    local letter = piece_letters[piece & 7]
    if is_black_piece(piece) then
        gfx.setColor(gfx.kColorBlack)
        gfx.fillCircleAtPoint(x + 15, y + 15, 10)
        gfx.setImageDrawMode(gfx.kDrawModeFillWhite)
    else
        gfx.setColor(gfx.kColorWhite)
        gfx.fillCircleAtPoint(x + 15, y + 15, 10)
        gfx.setColor(gfx.kColorBlack)
        gfx.drawCircleAtPoint(x + 15, y + 15, 10)
        gfx.setImageDrawMode(gfx.kDrawModeCopy)
    end
    local width = gfx.getTextSize(letter)
    gfx.drawText(letter, x + (CELL_SIZE - width) // 2, y + 8)
    gfx.setImageDrawMode(gfx.kDrawModeCopy)
end

local function draw_board()
    for row = 0, 7 do
        for column = 0, 7 do
            local x, y = column * CELL_SIZE, row * CELL_SIZE
            local square = screen_to_square(column, row)
            local file, rank = square & 7, square >> 4
            gfx.setColor(((file + rank) & 1) ~= 0 and gfx.kColorBlack or gfx.kColorWhite)
            gfx.fillRect(x, y, CELL_SIZE, CELL_SIZE)
            gfx.setColor(gfx.kColorXOR)
            if is_legal_target(square) then gfx.fillCircleAtPoint(x + 15, y + 15, 4) end
            if last_move and (last_move.from == square or last_move.to == square) then
                gfx.drawRect(x + 2, y + 2, CELL_SIZE - 4, CELL_SIZE - 4)
            end
            if selected == square then
                gfx.drawRect(x + 3, y + 3, CELL_SIZE - 6, CELL_SIZE - 6)
            end
            local piece = engine:get_piece(square)
            if piece ~= mcumax.EMPTY then draw_piece(square, piece) end
            if cursor == square then
                gfx.setColor(gfx.kColorXOR)
                gfx.drawRect(x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2)
            end
        end
    end
    gfx.setColor(gfx.kColorBlack)
    gfx.drawRect(0, 0, BOARD_SIZE, BOARD_SIZE)
end

local function draw_line(text, line)
    gfx.drawText(text, PANEL_X, 7 + line * 14)
end

local function draw_panel()
    draw_line("pdchess Lua", 0)
    draw_line((human_black and "Black" or "White") .. " / " ..
        difficulty_names[difficulty], 1)
    draw_line("Pos: " .. positions[position_index].name, 2)
    draw_line("Turn: " ..
        (engine:get_current_side() == mcumax.BLACK and "Black" or "White"), 3)
    draw_line("State: " .. engine:get_status(), 4)
    if last_move then draw_line("Move: " .. mcumax.format_move(last_move), 5) end
    if ai_thinking then
        draw_line("AI: thinking...", 7)
    elseif search_info then
        draw_line("Score: " .. search_info.score .. " cp", 7)
        draw_line("Nodes: " .. search_info.nodes, 8)
        draw_line("Depth: " .. search_info.depth, 9)
    end
    draw_line("D-pad cursor", 12)
    draw_line("A select/move", 13)
    draw_line("B cancel", 14)
    draw_line("Crank targets", 15)
    draw_line("Menu positions", 16)
end

local function draw_setup()
    gfx.drawText("pdchess Lua", 160, 42)
    gfx.drawText("mcu-max on Playdate", 138, 68)
    gfx.drawText("Up/Down side: " .. (human_black and "Black" or "White"), 112, 110)
    gfx.drawText("Left/Right AI: " .. difficulty_names[difficulty], 112, 136)
    gfx.drawText("A: open chess lab", 130, 178)
end

local function move_cursor(dx, dy)
    local file, rank = cursor & 7, cursor >> 4
    if human_black then dx, dy = -dx, -dy end
    file = math.max(0, math.min(7, file + dx))
    rank = math.max(0, math.min(7, rank - dy))
    cursor = rank * 16 + file
end

local function select_or_move()
    local status = engine:get_status()
    local test_position = position_index ~= 1
    local controlled_side = test_position and engine:get_current_side() or human_side()
    if (not test_position and engine:get_current_side() ~= human_side()) or
        status == "Checkmate" or status == "Stalemate" then
        return
    end

    if selected then
        for _, move in ipairs(legal_moves) do
            if move.from == selected and move.to == cursor then
                engine:play_move(move)
                last_move = move
                selected = nil
                refresh_legal_moves()
                ai_pending = not test_position and engine:get_status() ~= "Checkmate" and
                    engine:get_status() ~= "Stalemate" and
                    engine:get_current_side() ~= human_side()
                return
            end
        end
    end

    local piece = engine:get_piece(cursor)
    if piece ~= mcumax.EMPTY and
        (is_black_piece(piece) and mcumax.BLACK or mcumax.EMPTY) == controlled_side then
        selected = cursor
        crank_index = 0
    end
end

local function cycle_crank_target(direction)
    if not selected then return end
    local targets = {}
    for _, move in ipairs(legal_moves) do
        if move.from == selected then targets[#targets + 1] = move.to end
    end
    if #targets == 0 then return end
    crank_index = ((crank_index - 1 + direction) % #targets) + 1
    cursor = targets[crank_index]
end

local function run_ai()
    ai_thinking = true
    local best = engine:search_best_move(node_limits[difficulty], depth_limits[difficulty])
    search_info = engine:get_search_info()
    if best then
        engine:play_move(best)
        last_move = best
    end
    refresh_legal_moves()
    ai_pending = false
    ai_thinking = false
end

local menu = playdate.getSystemMenu()
menu:addMenuItem("Reset position", function() load_position(position_index) end)
menu:addMenuItem("Next position", function()
    load_position(position_index % #positions + 1)
end)
menu:addOptionsMenuItem("Test position",
    { "Start", "Castling", "En passant", "Promotion", "Mate", "Stalemate" },
    "Start", function(value)
        for index, position in ipairs(positions) do
            if position.name == value then
                load_position(index)
                return
            end
        end
    end)

function playdate.update()
    gfx.clear(gfx.kColorWhite)
    if screen == "setup" then
        draw_setup()
        return
    end

    if not ai_thinking then
        if playdate.buttonJustPressed(playdate.kButtonLeft) then move_cursor(-1, 0) end
        if playdate.buttonJustPressed(playdate.kButtonRight) then move_cursor(1, 0) end
        if playdate.buttonJustPressed(playdate.kButtonUp) then move_cursor(0, -1) end
        if playdate.buttonJustPressed(playdate.kButtonDown) then move_cursor(0, 1) end
        if playdate.buttonJustPressed(playdate.kButtonA) then select_or_move() end
        if playdate.buttonJustPressed(playdate.kButtonB) then selected = nil end

        crank_accumulator = crank_accumulator + playdate.getCrankChange()
        if crank_accumulator >= 8 then
            cycle_crank_target(1)
            crank_accumulator = 0
        elseif crank_accumulator <= -8 then
            cycle_crank_target(-1)
            crank_accumulator = 0
        end
    end

    draw_board()
    draw_panel()
    if ai_pending then run_ai() end
end

function playdate.upButtonDown()
    if screen == "setup" then human_black = not human_black end
end

function playdate.downButtonDown()
    if screen == "setup" then human_black = not human_black end
end

function playdate.leftButtonDown()
    if screen == "setup" then difficulty = (difficulty + 1) % 3 + 1 end
end

function playdate.rightButtonDown()
    if screen == "setup" then difficulty = difficulty % 3 + 1 end
end

function playdate.AButtonDown()
    if screen == "setup" then
        screen = "board"
        load_position(1)
    end
end

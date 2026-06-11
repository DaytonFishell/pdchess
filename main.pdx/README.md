# pdchess Lua

`pdchess` is a pure-Lua chess game for Playdate, powered by a Lua port of
mcu-max. It does not contain or compile any C code.

- `main.lua`: Playdate UI, controls, menus, and game flow.
- `mcumax.lua`: reusable chess rules and search engine.
- `pdxinfo`: Playdate package metadata.
- `tests/test_mcumax.lua`: desktop Lua regression tests.

The engine supports legal move generation, FEN import, castling, en passant,
queen promotion, check/checkmate/stalemate detection, and bounded iterative
search.

## Build

Install the Playdate SDK and set `PLAYDATE_SDK_PATH` when it is not located at
`V:\PlaydateSDK`:

```powershell
.\build-lua.ps1
```

The resulting package is `build\pdchess_lua.pdx`.

The build script stages only the Lua source and `pdxinfo`, keeping development
files out of the Playdate package.

## Test

With Lua 5.3 or newer:

```powershell
& C:\lua55\lua55.exe tests\test_mcumax.lua
```

The suite covers starting-position perft, move application, castling, en
passant, promotion, terminal positions, and bounded search.

## Controls

- Setup: Up/Down changes side, Left/Right changes AI difficulty, A starts.
- Board: D-pad moves the cursor, A selects or moves, B cancels selection.
- Crank: cycles through the selected piece's legal destinations.
- System menu: resets the game or loads special test positions.

## Engine API

```lua
local mcumax = require("mcumax")
local engine = mcumax.new()

local moves = engine:search_valid_moves()
local best = engine:search_best_move(15000, 5)
if best then
    engine:play_move(best)
end
```

Squares use mcu-max's 0x88 representation. Use `parse_square`, `parse_move`,
`format_square`, and `format_move` at API boundaries.

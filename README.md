# pdchess

`pdchess` is a compact, allocation-free chess engine module for Playdate C
projects. The library is the product; `demo/` is an interactive developer lab
showing how to integrate it.

The engine provides caller-owned instances, legal move generation, complete
FEN import/export, status inspection, and deterministic bounded search. It has
no dependency on Playdate graphics, input, audio, or application APIs, so host
tests can compile the same source used on device.

## Integration

With CMake:

```cmake
add_subdirectory(path/to/pdchess)
target_link_libraries(your_game PRIVATE pdchess)
```

Or compile `src/pdchess.c` directly and add `include/` to the include path.
The engine performs no heap allocation.

```c
pdchess_state game;
pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
pdchess_search_limits limits = {15000, 5, NULL, NULL, 0};

pdchess_init(&game);
size_t count = pdchess_generate_legal_moves(
    &game, moves, PDCHESS_MAX_LEGAL_MOVES);
pdchess_search_result result = pdchess_search(&game, limits);
if (result.has_move)
    pdchess_apply_move(&game, result.best_move);
```

## API Notes

- `pdchess_state` is opaque caller-owned storage. The current internal payload
  size is returned by `pdchess_state_size()`. In the verified v0.1.0 build the
  payload is 72 bytes inside 256 bytes of reserved public storage.
- Public squares use `a1 = 0` through `h8 = 63`.
- Each state is independent. Concurrent calls are safe when each thread uses a
  different state. Do not operate on one state concurrently.
- Search is synchronous and deterministic. Bound it by nodes and depth, and
  optionally use the polling callback for time limits or user cancellation.
- The polling callback runs inside search and must be fast. Return `false` to
  cancel.
- Move arrays are caller-provided. `PDCHESS_MAX_LEGAL_MOVES` is sufficient for
  all legal positions.
- Version 1 always promotes pawns to queens. Underpromotion is intentionally
  unsupported.
- Repetition and fifty-move draw adjudication are not implemented. FEN
  halfmove and fullmove counters are nevertheless preserved and updated.

Search currently uses approximately 1.4 KB of stack per active ply, mostly for
the fixed legal-move array. The developer lab caps its presets at depth 7, for
an estimated search stack below 10 KB. Confirm actual high-water usage on
hardware when integrating unusually deep search settings.

## Host Tests

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Playdate Lab

Configure `demo/` as a normal Playdate C project:

```powershell
$env:PLAYDATE_SDK_PATH = "V:\chess-playdate\PlaydateSDK"
cmake -S demo -B demo/build -G "Visual Studio 17 2022" -A x64
cmake --build demo/build --config Debug
```

For hardware, install `gcc-arm-none-eabi` and configure with:

```powershell
cmake -S demo -B demo/build-device `
  -DCMAKE_TOOLCHAIN_FILE="$env:PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake"
cmake --build demo/build-device --config Release
```

The lab controls and test positions are described on screen. It deliberately
avoids production-game features such as persistence, clocks, animations,
opening books, and polished menus.

The Windows simulator build was verified with Playdate SDK 3.0.6. The resulting
debug native payload was 88,064 bytes. A device build additionally requires
`arm-none-eabi-gcc`; that compiler is not bundled with the Windows SDK.

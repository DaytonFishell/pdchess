# Third-Party Notices

`pdchess` is an original instance-based adaptation informed by the compact
board and bounded-search design of:

- **mcu-max 1.0.6**, Copyright (c) 2022-2025 Gissio, MIT License.
- **micro-Max 4.8**, by H.G. Muller.

The unmodified mcu-max source and its license are available in the sibling
`mcu-max` checkout used as the reference for this port.

The public API, caller-owned state model, legal-rules layer, FEN
serialization, diagnostics, tests, and Playdate developer lab were written
for this project. Like mcu-max, version 1 supports queen promotion only.

# Licensing and attribution

## This project is GPL-2.0

`tdoom/src/doomgeneric/` is the Doom engine, carried here from
[doomgeneric](https://github.com/ozkl/doomgeneric) (which descends from
Chocolate Doom, which descends from id Software's 1997 Doom source release).
Those files carry:

> Copyright(C) 1993-1996 Id Software, Inc.
> Copyright(C) 2005-2014 Simon Howard
>
> This program is free software; you can redistribute it and/or modify it under
> the terms of the GNU General Public License as published by the Free Software
> Foundation; either version 2 of the License, or (at your option) any later
> version.

Because the platform layer is linked into that engine, **the project as a whole
is distributed under the GPL-2.0** — see `LICENSE`. Local modifications to the
engine are marked with a `tdoom:` comment explaining what changed and why.

## Vendored third-party code

- **`libraries/Arduino_GFX/`** — [Arduino_GFX](https://github.com/moononournation/Arduino_GFX)
  by Moon On Our Nation, BSD licence (see `libraries/Arduino_GFX/license.txt`).
  Vendored rather than installed because macOS TCC hides
  `~/Documents/Arduino/libraries` from `arduino-cli`; see `CLAUDE.md`.

  Two local changes:
  - `src/databus/Arduino_ESP32QSPI.h` — transfer chunk size reduced, with the
    reasoning in the file.
  - `src/Arduino_GFX.h` and `src/font/` — six large CJK/unifont headers removed
    (~16MB, unreferenced). Restore from upstream if you need CJK text.
  - `examples/` removed; `arduino-cli` never compiles a library's examples.

## Game data is NOT included

`doom1.wad` is not in this repository. The shareware WAD is freely
redistributable, but it is id Software's data, not ours to vendor — supply your
own. The full `doom.wad` requires a purchased copy of Doom.

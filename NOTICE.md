# Notice

## Relation to upstream

This is an independent, non-commercial port of
[MacNativeGames](https://github.com/ly05010419/MacNativeGames) to Android.
`runtime/gate.S`, the module loader, the relocation pass and the NVN seam are
ported from that project (MIT). The graphics backend is not: it is written for
Vulkan instead of Metal.

Not affiliated with, endorsed by, or sponsored by Nintendo.
*The Legend of Zelda* and *Nintendo Switch* are trademarks of Nintendo.

## No game content

This repository contains **no game code, no keys, no firmware, no textures, no
audio, no shaders and no saves**, and distributes none. Everything the game
needs is produced on the player's own machine from files the player owns:

    tools/import_totk.py <your game.xci> -k <your prod.keys>

which writes into a folder *outside* this repository. The app you build
contains the game, so it is for your own use only — do not share it.

`tools/check_provenance.py` enforces this and fails if anything game-derived
ever lands in the tree.

## Third-party

| | |
|---|---|
| MacNativeGames | MIT, the ported C sources |
| Ryujinx shader translator | MIT, vendored by upstream, produces the SPIR-V |
| SPIRV-Cross | Apache-2.0, upstream only (not needed on Android) |
| Nintendo Switch firmware, prod.keys | not included, never will be |

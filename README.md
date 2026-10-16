# ReyondBroom

[![Download for Windows x64](https://img.shields.io/badge/Download_for_Windows_x64-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Windows.zip)
[![Download for Linux x86_64](https://img.shields.io/badge/Download_for_Linux_x86__64-FCC624?style=for-the-badge&logo=linux&logoColor=black)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Linux-x86_64.AppImage)

[All downloads, SHA-256 checksums and release notes](https://github.com/reyond9079/ReyondBroom/releases/latest)

<img width="1024" height="559" alt="image" src="https://github.com/user-attachments/assets/a5c4d85d-7bad-4b55-943c-c7546aa6bc71" />


A custom modification of Trenchbroom.

**English** · [Russian](README.ru.md)

ReyondBroom is a TrenchBroom v2026.2-based toolset for creating and placing brush rubble.
Development happens on the [`ReyondBroom` branch](https://github.com/reyond9079/ReyondBroom/tree/ReyondBroom).

- **[Perforate](PERFORATE.md)** creates random polygonal through-holes, with exact
  counts, side counts, size ranges, spacing and optional selected `func_wall` plugs.
- **[Fracture](FRACTURE.md)** splits one brush into 2–128 convex Voronoi fragments,
  with a seed, cut material, wireframe preview and a single Undo operation.
- **[Settle](SETTLE.md)** simulates gravity and collisions for 1–128 selected brushes,
  with friction, bounce, an initial kick, Pause/Resume and undoable placement.

## Quick start

Download [ReyondBroom for Windows x64](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Windows.zip).
Extract the ZIP into a separate folder and run `TrenchBroom.exe`.
The executable retains the upstream editor name.

For **Linux x86_64**, download the [AppImage](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Linux-x86_64.AppImage).
It is built and tested on Ubuntu 24.04 with Qt included; older distributions are not verified.
Make it executable and launch it:

```sh
chmod +x ReyondBroom-Linux-x86_64.AppImage
./ReyondBroom-Linux-x86_64.AppImage
```

If FUSE is unavailable, launch with `--appimage-extract-and-run`.

Select a brush → **Edit → CSG → Fracture…** → **Apply**. Keep the fragments selected,
then choose **Edit → CSG → Settle…** → **Simulate / Restart** → **Apply**.
Create an unselected floor brush first; Settle does not add an automatic floor.

See [AGENTS.md](AGENTS.md) for contributor conventions and [BUILD.md](BUILD.md) for
build prerequisites. Fracture and Settle remain experimental; validate generated
geometry with your game's BSP compiler. The upstream GPL license is preserved.

## Extended editing tools

This branch integrates the supported features of
[UltimateTrenchBroom](https://github.com/dragxnd/UltimateTrenchBroom), authored by
dragxnd and based on TrenchBroom. See [the integration guide](ULTIMATE.md)
([Russian](ULTIMATE.ru.md)) for the feature inventory and compatibility notes.

## Contributors and license

- dragxnd: UltimateTrenchBroom tools, workflow design and documentation.
- Kristian Duske and the TrenchBroom contributors: original editor and infrastructure.

ReyondBroom retains upstream copyright notices and the
[GPL-3.0-or-later license](LICENSE.txt).

## Upstream TrenchBroom



[![Original TrenchBroom Icon](docs/assets/upstream-icon.png)](https://www.youtube.com/watch?v=shcAvnYp9ow)

TrenchBroom is a modern cross-platform level editor for Quake-engine based games.

- Trailer:   https://www.youtube.com/watch?v=shcAvnYp9ow
- Website:   https://github.com/TrenchBroom/TrenchBroom
- Discord:   https://discord.gg/WGf9uve
- Mastodon:  https://mastodon.gamedev.place/@trenchbroom
- Bluesky:   https://bsky.app/profile/trenchbroom.bsky.social
- Video Tutorial Series:  https://www.youtube.com/playlist?list=PLgDKRPte5Y0AZ_K_PZbWbgBAEt5xf74aE
- Manual:    https://trenchbroom.github.io/manual/latest

## Features
* **General**
  - Full support for editing in 3D and in up to three 2D views
  - High performance renderer with support for huge maps
  - Unlimited Undo and Redo
  - Macro-like command repetition
  - Issue browser with automatic quick fixes
  - Point file support
  - Automatic backups
  - .obj file export
  - Free and cross platform
* **Brush Editing**
  - Robust vertex editing with edge and face splitting and manipulating multiple vertices together
  - Clipping tool with two and three points
  - Scaling and shearing tools
  - CSG operations: merge, subtract, intersect
  - UV view for easy texture manipulations
  - Precise texture lock for all brush editing operations
  - Multiple material collections
* **Entity Editing**
  - Entity browser with drag and drop support
  - Support for FGD and DEF files for entity definitions
  - Mod support
  - Entity link visualization
  - Displays 3D models in the editor
  - Smart entity property editors
* **Supported Games**
  - Quake (Standard and Valve 220 file formats)
  - Quake 2
  - Quake 3 (partial, no patches or brush primitives yet)
  - Hexen 2
  - Daikatana
  - Generic (for custom engines)
  - More games can be supported with custom game configurations


## Releases

Binary builds are available from [releases](https://github.com/kduske/TrenchBroom/releases).

## Compiling

Read [BUILD.md](BUILD.md) for instructions.

# Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for more information.

# Credits
- [Qt](https://www.qt.io/)
- [FreeType](https://www.freetype.org/)
- [FreeImage](https://freeimage.sourceforge.io/)
- [TinyXML](http://www.grinninglizard.com/tinyxml/)
- miniz
- [Assimp](https://www.assimp.org/)
- [Catch2](https://github.com/catchorg/Catch2)
- [CMake](https://cmake.org/)
- [vcpkg](https://www.vcpkg.io/)
- [Pandoc](https://www.pandoc.org/)
- Quake icons by [Th3 ProphetMan](https://www.deviantart.com/th3-prophetman)
- Hexen 2 icon by [thedoctor45](https://www.deviantart.com/thedoctor45)
- [Source Sans Pro](https://fonts.google.com/specimen/Source+Sans+Pro) font

## Changes

See [releases](https://github.com/TrenchBroom/TrenchBroom/releases) for latest changes.

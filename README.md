# ReyondBroom

**A standalone 3D level editor for brush-based games.** Build spaces, shape architecture, texture surfaces and place gameplay entities in one workspace—from a first room to a detailed playable map.

[![Download for Windows x64](https://img.shields.io/badge/Download_for_Windows_x64-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Windows.zip)
[![Download for Linux x86_64](https://img.shields.io/badge/Download_for_Linux_x86__64-FCC624?style=for-the-badge&logo=linux&logoColor=black)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Linux-x86_64.AppImage)

**English** · [Русский](README.ru.md) · [Illustrated handbook](https://reyond9079.github.io/ReyondBroom/) · [Full reference](https://reyond9079.github.io/ReyondBroom/reference.html) · [Releases and checksums](https://github.com/reyond9079/ReyondBroom/releases/latest)

![ReyondBroom: convex Bevel preview in the map viewport](docs/captures/reyond/bevel-preview.png)

## Build the whole level

- **Edit in 3D and 2D.** Create brush primitives, move vertices, edges and faces, clip, extrude, sweep, scale and shear. Work with Quake 3 patches and control points in supported map formats.
- **Shape architecture and terrain.** Draw polygon cuts and volumes, inset faces, bend groups, extrude curved chains, subdivide surfaces, sculpt terrain and generate rocks.
- **Round and break geometry.** Bevel face boundaries, edges or vertices with radius, segments, convex profiles and Clamp. Fracture solids into seeded fragments, perforate surfaces and settle pieces with gravity and collisions.
- **Repeat and align.** Place linear, radial or random arrays, synchronize symmetric copies, rotate the grid, snap vertices to integers and enter exact XYZ coordinates.
- **Texture and organize.** Edit UV alignment, browse materials, load texture folders, choose WAD export palettes, place prefabs and navigate with EntityList. Groups, linked groups and layers keep large maps manageable.
- **Preview and export.** Inspect approximate Quake lighting, sky, moving entities and train routes. Configure external compilers, inspect issues and leak traces, launch the game or synchronize geometry and textures to Unity.

ReyondBroom is independently maintained, with its own releases and documentation. It installs as a complete application; another editor is not required.

## Get started

1. **Install.** On Windows, extract the entire ZIP and run `TrenchBroom.exe`—the current executable filename. Keep its resources and DLLs. On Linux, make the AppImage executable and launch it; use `--appimage-extract-and-run` if FUSE is unavailable.
2. **Choose your game.** Create a map, select a compatible format, and configure the game directory, mod and entity definitions. Game data and compilation tools are supplied separately.
3. **Build a room.** Start with a floor, four walls and a ceiling. Add a player start and a light, apply materials and save.
4. **Compile and test.** Configure **Run → Compile** with your game's tools. Inspect the result in the engine before adding detail.

[Follow the illustrated first-map walkthrough →](https://reyond9079.github.io/ReyondBroom/#first-room)

Windows x64 and Linux x86_64 binaries are available. The Linux AppImage is built and tested on Ubuntu 24.04; older distributions are not verified. A macOS binary is not provided in the current release.

## Learn the tools

| What you want to do | Guide |
| --- | --- |
| Navigate, select, build and texture | [Start here](https://reyond9079.github.io/ReyondBroom/#start) |
| Round edges and corners | [Bevel](https://reyond9079.github.io/ReyondBroom/#bevel) |
| Break a wall and place rubble | [Fracture](https://reyond9079.github.io/ReyondBroom/#fracture) · [Settle](https://reyond9079.github.io/ReyondBroom/#settle) |
| Cut random through-holes | [Perforate](https://reyond9079.github.io/ReyondBroom/#perforate) |
| Generate terrain and curved structures | [Terrain](https://reyond9079.github.io/ReyondBroom/#terrain) · [Bezier Bend](https://reyond9079.github.io/ReyondBroom/#bezier-bend) · [Spin](https://reyond9079.github.io/ReyondBroom/#spin) |
| Repeat modules and reuse assets | [Array](https://reyond9079.github.io/ReyondBroom/#array-line) · [Symmetry](https://reyond9079.github.io/ReyondBroom/#symmetry) · [Prefabs](https://reyond9079.github.io/ReyondBroom/#prefabs) |
| Compile, diagnose and export | [Compilation](https://reyond9079.github.io/ReyondBroom/#compile) · [Unity Sync](https://reyond9079.github.io/ReyondBroom/#unity-sync) |

The handbook has matching English and Russian chapters, screenshots, searchable navigation, steps and limitations. The full technical reference covers the core editor, expressions and game configuration in English. [Documentation sources and local build instructions](docs/README.md) are included.

## Games and formats

Game configurations cover Quake, Quake 2, Quake 3, Hexen 2, Daikatana and custom engines. Map formats, materials, entity classes and patch support depend on the selected configuration. Quake Standard and Valve 220 workflows are supported. Choose a format your compiler accepts; a viewport preview does not replace testing the compiled map.

## Development and reference

Development takes place on **ReyondBroom**. See [BUILD.md](BUILD.md), [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md) for build instructions and repository conventions.

Source projects and image provenance are recorded once in [Sources and licenses](https://reyond9079.github.io/ReyondBroom/sources.html). The editor is distributed under [GPL-3.0-or-later](LICENSE.txt); source notices and [third-party licenses](THIRD_PARTY_NOTICES.md) are retained.

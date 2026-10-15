# Extended tools in ReyondBroom

**English (primary)** · [Russian](ULTIMATE.ru.md)

ReyondBroom integrates the supported feature set from
[dragxnd/UltimateTrenchBroom](https://github.com/dragxnd/UltimateTrenchBroom),
revision `e093dc9cd41c2e9beb00a9ed0d0dfa2241e4a666`. The original authorship,
GPL license, source history and third-party notices are retained.

## Feature inventory

| Area | Tools and capabilities |
| --- | --- |
| Geometry | Terrain generation; Polygon Cut and Polygon Spawn; multi-edge Bevel; face Inset and Spin; Bezier Bend; Subdivision; Subtract 2; Convert Triang Brushes; Generate Rock; vertex Sculpt |
| Placement | Array with line, star, ring and random placement; Symmetry with 2/4/8 sides and rotation |
| Precision | Rotated face grids and snapping; integer vertex snapping; editable XYZ overlay |
| Assets | Prefab collections with previews and placement; searchable EntityList; material and entity thumbnail zoom; collapsible material groups and texture refresh |
| Textures | Texture Folders; Pallete selection and preview; compile-time WAD generation |
| Export | Unity Sync with geometry, aligned textures, root scale, optional brush children and material reuse |
| Quake preview | Lighting, sky, door/button/lift/train animation, property updates and intermission camera |
| Quake workflow | Spawn Master for triggered teleport spawns and a shared monster closet |
| Interface | Live tool panels, Apply/Default controls, Russian keyboard movement and guide screenshot capture |

The imported [visual guide](docs/index.html) explains the individual
tools. Its screenshots and original product name describe UltimateTrenchBroom;
ReyondBroom keeps its own executable name, `TrenchBroom.exe`.

## Compatibility

This integration retains the TrenchBroom v2026.2 base, including its unified
vertex/edge/face handle selection, multiple shortcuts, control-point and Sweep
tools, and adaptive grid rendering. Imported tools use these newer interfaces.
Fracture, Perforate and Bullet Settle remain available. `func_wall` fragments
continue to collide in Settle, including with visible Quake 3 patches.

The experimental MCP bridge removed by UltimateTrenchBroom and its hidden,
unsupported model import are not part of this feature inventory. Quake-specific
entity and preview features depend on the selected game's definitions and assets.

Geometry tools remain experimental. Work on a copy of important maps and check
the resulting geometry with the game's BSP compiler. Unity Sync requires a Unity
project; its exported result must also be checked there.

## Windows build and checks

Download `ReyondBroom-Windows` from a successful
[Windows workflow](https://github.com/reyond9079/ReyondBroom/actions/workflows/fracture.yml?query=branch%3AReyondBroom).
Extract both the downloaded artifact and its enclosed ZIP, then run
`TrenchBroom.exe` with the accompanying resources and DLLs.

The workflow builds the complete editor and model/UI test targets. Focused tests
cover geometry, map operations, handle selection, action conflicts, tool panels,
guide capture and the ReyondBroom rubble tools. Automated tests do not replace
checking rendering and game-specific output in the editor and target engine.

# Settle Brushes

Select 1–128 brushes, then choose **Edit → CSG → Settle…**. This native tool is
included alongside Fracture in this branch, based on TrenchBroom v2026.2.

**Simulate / Restart** starts an animated isometric wireframe preview. Visible,
unselected brushes are static collision obstacles, including locked brushes.
Hidden brushes, patches and point entities are ignored. There is no implicit
ground: create a floor in the map. Up to 4096 visible obstacle brushes are supported.

- **Friction** and **Bounce** control contact behavior (0–1).
- **Initial kick** adds seeded horizontal/upward velocity and rotation. Set 0 to
  drop from rest. Touching fracture pieces may need a kick to spread into rubble.
- **Duration** is the maximum simulated time (0.1–20 seconds).
- **Seed** reproduces the kick in the same build; bit-identical results across
  compilers or physics library versions are not guaranteed.
- **Pause / Resume** lets you freeze a particular moment. Apply can capture moving
  pieces as well as a sleeping pile; the status explicitly distinguishes these.

Changing a parameter discards the previous preview. **Apply** writes the validated
brushes back into their existing nodes. Parents, entity membership and selection
are retained; one **Undo** restores the originals. Cancel never edits the map.
Texture locking uses the existing Brush transform operation. Valve UV mapping can
represent arbitrary rotations; classic map UV mapping has its existing limitations.

## Physics and limits

Bullet's convex hull rigid bodies use volume-derived masses and approximate hull
inertia. Map coordinates are scaled by 1/32 for physics. Gravity is 800 map units/s²,
integration is fixed at 240 Hz, with continuous collision detection and 40 solver
iterations. Small contact margins are 0.02 map units. Contacts deeper than 0.25 map
units block Apply. Initial deep overlaps also block simulation. Brushes that leave
world bounds or become invalid after rotation cannot be applied. Geometry is never
snapped to the grid after settling, as that would deform fragments and contacts.

This is an editor placement tool, not runtime destruction or a watertight union.
Physics contacts have numerical tolerances. Especially thin, tiny or densely stacked
pieces may require a lower kick, fewer pieces or a different seed. No guarantee is
made about a particular BSP compiler or game; compile and inspect the resulting map.

## Building and validation

Bullet is supplied by the `bullet3` vcpkg dependency. For system dependency builds,
install the Bullet development package as well. The portable Windows archive includes
Bullet's license; Bullet itself is linked statically by vcpkg on Windows.

Focused model tests: `TbMdlLibTest "BrushFracture,BrushSettle,Map_Geometry"`.
Focused UI tests: `TbUiLibTest "FractureDialog,SettleDialog"`.

Tests cover gravity, floor and wall contact, stacking, thin-floor CCD, seed
repeatability, fracture-to-settle, rigid transforms, Valve texture locking, invalid
input, escape from world bounds, dialog lifecycle and map undo/redo/serialization.

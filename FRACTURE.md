# ReyondBroom — Voronoi brush fracture

**English (primary)** · [Русский](FRACTURE.ru.md) · [Overview](README.md)

This experimental branch is based on TrenchBroom **v2026.2**.

1. Select exactly one brush, with no modal editing tool active.
2. Open **Edit > CSG > Fracture...**.
3. Set **Fragments** (2–128), **Seed**, and **Cut material**.
4. Click **Generate Preview**, or **Another Variant** to increment the seed.
5. **Separation** defaults to 0 (no gaps). A nonzero value moves each fragment
   outward from the source center; the preview and Apply use the same geometry.
   100 means an additional displacement equal to the original center offset.
   **No collide** creates one `func_wall` per fragment. It does not change any
   mutual or environment collisions in Settle.
6. Click **Apply**. The pieces replace the selected brush under its original
   parent and are selected. One **Undo** restores the original brush and selection.
   **Cancel** leaves the document untouched.

Changing a geometry parameter invalidates the preview and disables Apply until it has
been regenerated successfully. The exact preview geometry is applied; it is not
randomly generated a second time. Separation updates immediately from the cached
fragments, without accumulating movements; invalid world bounds disable Apply. Exterior face material and UV projection are
preserved, including x/y texture scales. Internal faces inherit attributes from
a source face matching the cut material, or the first face if none matches.
The original parent is retained unless separate func_wall entities are requested;
these are placed beside the source entity if necessary to avoid nesting entities.
The fragment brushes themselves are selected. No game physics is added.

## Geometry

Sites are sampled inside a volume-weighted tetrahedral decomposition of the
convex brush using a seeded `mt19937`. Each Voronoi cell is the intersection of
the original brush and the bisector half-spaces of its site against every other
site. Both sides of each bisector use the same plane points with reversed winding.
Clipping uses TrenchBroom's existing brush geometry code.

The operation fails as a whole if clipping fails, a cell is incomplete or too
small, sites cannot be placed, or total volume exceeds the allowed error. No
fragment is silently dropped. Sites need 0.01 map units of clearance, and pieces
need at least 0.001 cubic map units. These numerical guards are not a gameplay
minimum-size setting. Very small/thin brushes may need fewer fragments.

The first version has an isometric wireframe dialog preview, not an interactive
viewport gizmo. Impact-point control and anisotropy are outside its scope. Many pieces can increase BSP compile costs; validate the
result with the compiler used by your game.

## Validation

`TbMdlLibTest "BrushFracture,Map_Geometry"` covers known one/two/eight-site
partitions, exact counts, volume conservation, containment, pairwise intersections,
Voronoi nearest-site inequalities, seeded reproducibility, Standard/Valve formats,
exterior UV retention, invalid sites, a rotated thin brush far from the origin,
replacement, Undo/Redo, and map serialization/parsing.

`TbUiLibTest "FractureDialog"` checks preview invalidation, material validation,
seed regeneration, applying exactly the generated geometry, and cancellation.

TrenchBroom corrects coordinates within 0.001 map units of an integer. The runtime
plane-distance tolerance accounts for that three-dimensional displacement, and
the volume tolerance scales with the source surface area. Results are numerical
brush geometry, not exact symbolic solids.

The branch also contains a Windows workflow that builds the editor and model
tests, runs these tests, and packages the editor as a downloadable Actions artifact.
See BUILD.md for the normal local build prerequisites.

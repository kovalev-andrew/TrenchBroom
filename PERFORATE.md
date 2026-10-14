# ReyondBroom — Perforate Brush

**English (primary)** · [Русский](PERFORATE.ru.md) · [Overview](README.md)

Select exactly one brush face, then open **Edit → CSG → Perforate…**.
Holes go through the entire brush, perpendicular to that face.

- **Holes:** 1–64. Every requested hole must fit.
- **Contour sides:** 3–32 sides per random convex polygon, excluding end caps.
- **Minimum / maximum diameter:** map units, at least 4, minimum ≤ maximum.
  Diameter is the largest distance between two vertices of the 2D contour.
  Each hole gets its own random diameter within the range. Equal values fix it.
- **Minimum spacing / edge margin:** at least 1 map unit. Keeps holes apart
  and inside the selected face. Packing uses conservative bounding circles.
- **Seed:** repeatable geometry in the same build.
- **Keep plugs:** enabled by default. Each hole's clipped plug is one brush in
  its own `func_wall`, and only the plug brushes are selected after Apply.
  When disabled, plugs are removed and remaining brushes are selected.

**Generate preview** computes the entire operation. The preview looks along the
selected face normal; orange outlines show the holes. Apply is enabled only for
a valid result. Changing geometry parameters invalidates it. **Apply** replaces
the source; one **Undo** restores the source and face selection. Cancel leaves
the map unchanged. Plugs retain normal Settle collisions, just like other brushes.

Random angular samples on a rotated ellipse produce convex contours. Each contour
is extruded through the source and clipped to it, producing one plug brush. The
plugs are subtracted using TrenchBroom CSG. Since a brush is convex, material around
holes becomes multiple convex brushes. Existing exterior face attributes are
retained; hole sides inherit the selected face's attributes and material.

If all requested holes cannot be placed, a contour loses sides, subtraction fails,
the total volume check fails, or more than 4096 remaining brushes are needed,
the operation is rejected. Counts and size ranges are never silently reduced.
Try another seed, smaller holes, fewer holes, fewer sides or more clearance.
This bounded randomized search is not a proof that every rejected layout is
mathematically impossible. Nonparallel back faces may produce irregular end caps;
the requested side count describes the 2D contour, not the total plug face count.

`TbMdlLibTest "BrushPerforate,Map_Geometry"` tests counts, contour sides, sizes,
through direction, nonintersection, reproducibility, invalid layouts, selection,
func_wall wrapping and Undo/Redo. `TbUiLibTest "PerforateDialog"` tests preview
invalidation, Keep plugs and failure recovery. Standard, Valve and Quake 3 geometry
are covered; game support for `func_wall` depends on the selected game/mod.

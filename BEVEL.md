# Bevel

**English** · [Russian](BEVEL.ru.md)

Bevel rounds brush edges and vertices while keeping each source brush as one convex brush.
The preview appears directly in the map views. No preview window or physics simulation is used.

## Selection

- **Face boundary:** select surfaces first, then open **Edit → Tools → Bevel**. Only edges belonging to exactly one selected surface are beveled. Shared edges within the selected region are excluded. Selecting every face of a closed brush leaves no boundary.
- **Vertices:** use the vertex tool to select corners, then open Bevel. The selection is captured before switching tools.
- **Edges:** select edges before opening Bevel, or select them in the Bevel tool before changing a parameter.

The mode menu uses the selection captured when the tool opened. Cancel and select again to change the target surfaces or vertices. Geometry dragging is disabled during Bevel.

## Controls

- **Radius:** radius of the circular reference profile, in map units. **Offset** measures the setback on the adjacent surfaces for edges, and the reference corner setback for vertices. On oblique corners these measurements differ.
- **Segments:** 1–16 subdivisions across an edge profile. One produces a flat chamfer. Vertex patches need additional faces to close the corner; this is not a total face-count limit.
- **Profile:** convex shapes only. 0 is flat, 0.5 is round, values approaching 1 make the profile squarer. The maximum is 0.99 to avoid the degenerate unmodified corner. Changing the profile changes the reference shape; noncircular profiles do not have one constant radius. This parameterization is not numerically identical to Blender's Profile slider.
- **Clamp per brush:** independently reduces the requested size for each brush. It checks remaining face areas and the intervals between caps on original edges. Other brushes in the scene are not obstacles. The status line lists the applied size of each limited brush.
- **Apply:** commit the displayed result as one undoable operation. Resulting brushes remain selected.
- **Cancel / Escape:** restore the original geometry. Switching tools also cancels the preview. Undo is held while the preview is active; apply or cancel first.

All recalculations start from the captured source, avoiding cumulative rounding errors. Invalid settings restore the source and disable Apply; they never partially replace a group of brushes. Turning Clamp off does not permit invalid or overlapping bevels.

## Geometry and limitations

Results use planar brush faces, not smooth mesh normals or Quake 3 patches. Edge intersections use convex corner caps. Vertex mode uses spherical reference caps; at corners with more than three incident edges a least-squares reference center is used, so equal tangency to every edge is not guaranteed. This is a brush-oriented implementation, not a port of Blender's BMesh topology or its miter options.

Clamp retains a small precision margin instead of creating zero-area faces or zero-length edges at exact contact. Very small details can be rejected by the brush geometry kernel. The operation limits a brush to 128 selected elements and 4096 candidate cutting planes; use fewer segments for complex corners.

Original surfaces retain their texture attributes and UV coordinate systems. New bevel faces inherit material and numeric texture attributes from an adjacent surface; seamless UV wrapping across all curved corners is not guaranteed.

Tests cover face-boundary selection, circular radius, convex profiles, vertex caps, per-brush Clamp, texture preservation, non-axis-aligned brushes, preview cancellation and Undo/Redo. Validate maps with the BSP compiler for the target game.

# GLB Exporter Fix Spec — bones-at-mesh alignment

This document describes a fix needed in **the external exporter** (the
separate program/plugin that produces `.glb` files consumed by
HoloRoll), not in HoloRoll itself. Paste this whole document into the
exporter project's AI assistant or apply manually.

Sample broken file: `test.glb` (3.1 MB, vehicle interior, 169 joints).

---

## What the problem looks like

In any **canonical glTF viewer** (Khronos Sample Viewer, three.js
editor, Babylon.js sandbox, HoloRoll, etc.), the bone visualisation
for the exported `.glb` shows joint pivots **offset from the mesh body
by ~1–3 metres**. Bones do not appear "on" the mesh. Skinned vertices
DO deform correctly (the mesh animates), but the bones are visibly in
the wrong place.

In Blender's own importer the bones DO align with the mesh after
re-import. This is because Blender's `io_scene_gltf2` does extra
processing (`move_skinned_meshes` + `skin_into_bind_pose` + a separate
VNode bone-hierarchy chain — see
[glTF-Blender-IO](https://github.com/KhronosGroup/glTF-Blender-IO)).
Canonical viewers follow the glTF 2.0 spec literally and DON'T do this
extra processing.

## Validator output (Khronos glTF Validator)

```
Warning  NODE_SKINNED_MESH_NON_ROOT  Node with a skinned mesh is not root.
                                     Parent transforms will not affect a
                                     skinned mesh.
                                     /nodes/261

Info  NODE_EMPTY  Empty node encountered. (× ~30 nodes)
```

## Diagnostic numbers

From running `scripts/diagnose_glb.py` on the broken `.glb`:

- Mesh node has a `-90°X` rotation (Z-up → Y-up axis conversion baked
  in the parent chain) — typical Blender export.
- Skinned mesh bake produces vertices at scene-world centre
  `(-0.897, 2.39, -2.14)`.
- Joint 0 (`control_piece_015`) world position `(-0.42, 0.68, 0.35)`.
- TOP deformation joint (`seat_back`, weight sum 13122) at
  `(-0.055, 0.86, -0.055)`.
- Distance from `seat_back` to mesh centre: **~2.7 metres**. Way off.

The model has 169 joints, **all** with non-zero deformation weight,
mostly clustered around `(0, 0.86, 0)` (the rig's local origin).
Joint nodes are placed at **animator-rig pivot positions**, not at the
mesh body positions they actually deform.

## Why this happens (rig structure)

The rig uses an "animator-friendly" pivot structure: control nodes
(`anim_seat_rot`, `anim_seat_mov`, `ANIM_control_geo_right08`,
`Control Geo_Right`, …) sit at convenient world positions for the
animator to grab. Bones that actually deform the mesh are children of
this pivot chain.

Blender authoring view shows bones AT mesh body because Blender draws
edit-bones using `inv(IBM)` (not `node.globalTransform`). Canonical
viewers draw joints at `node.globalTransform` — which for this rig is
the pivot position, not the mesh-body position.

---

## How to fix the exporter

Pick **one** of three paths. **Path A is recommended** — most
compatible with all viewers, cleanest data.

### Path A — Reposition joint nodes to weighted vertex centroids

For each joint `j` that will be included in `skin.joints[]`:

1. Compute the **weighted vertex centroid** for joint `j`:
   ```
   centroid[j] = Σ_v (weight[v][j] * vertex_world_pos[v])
               / Σ_v weight[v][j]
   ```
   where `vertex_world_pos[v]` is the mesh vertex position in the
   mesh node's world frame (i.e. after applying `mesh_node_world` to
   the mesh-local vertex), and `weight[v][j]` is the skin weight of
   vertex `v` on joint `j`.

2. Compute the **rotation** the joint should have at bind pose:
   - Either keep its current rotation (if the rig's joint orientations
     are already correct for the deformation), OR
   - Reorient so bone forward (typically `-Z` or `+Y` in joint local)
     points along the natural axis of the deformed region.

3. Set `joint_node.translation`, `joint_node.rotation`,
   `joint_node.scale` such that `joint_node.globalTransform` produces
   the new bind pose:
   ```
   joint_world_bind_new = T(centroid[j]) * R(orientation)
   joint_local_TRS = inv(parent.globalTransform) * joint_world_bind_new
   ```
   then decompose into T, R, S.

4. **Recompute the IBM** so skinning still produces the same vertex
   positions as before:
   ```
   IBM_new[j] = inv(joint_world_bind_new) * mesh_node_world
   ```
   (The `* mesh_node_world` factor is the Blender convention — keeps
   skinning math `joint_world * IBM * vertex_local` producing scene-
   world results.)

5. **Re-bake animation channels** for the joint. The animation TRS
   on each joint node was authored relative to the OLD bind pose.
   Convert to be relative to the NEW bind pose:
   ```
   for each keyframe (T_old, R_old, S_old):
       world_at_t_old = parent_world_at_t * compose(T_old, R_old, S_old)
       # Convert to the new local frame:
       local_to_new_parent = inv(parent_world_at_t) * world_at_t_old
       T_new, R_new, S_new = decompose(
           inv(joint_world_bind_new_local_to_parent) * local_to_new_parent
       )
   ```
   The mesh deformation at every animated frame stays identical to
   what the original rig produced; only the joint node position
   changes.

**Pros:** bones land on mesh in every canonical viewer; rig hierarchy
is preserved; animator-facing transform values change but visual
result is identical.

**Cons:** moderate rewrite of the export pass; needs the weights AND
animation channels to be jointly processed.

### Path B — Filter to deformation bones (simpler, if rig allows)

If the source rig has a "Deform" / "is-deform-bone" flag (Blender bones
do), exclude non-deform control nodes from `skin.joints[]`:

1. In your bone-iteration during export, filter to bones where
   `bone.use_deform == True` (Blender) or equivalent.
2. Skip control / IK / pivot bones — they shouldn't be in
   `skin.joints[]` at all.
3. For bones that ARE included, their `node.translation` should be
   the bone's **HEAD position** (where Blender draws the bone start),
   not some intermediate pivot.

**Pros:** much smaller change, fewer joints exported (cleaner file).

**Cons:** breaks if the rig genuinely USES control bones for
deformation (i.e. control bones have non-zero skin weights on
vertices). For test.glb specifically this would break the animation
because all 169 joints have weight contribution.

### Path C — Set `skin.skeleton` (low-impact, partial)

Add `skin.skeleton = N` where `N` is the index of the common ancestor
node of all joints. Doesn't fix the bones-at-pivot problem on its own,
but helps importers normalize the skeleton hierarchy. Often combined
with Path A or B.

**Pros:** trivial change.

**Cons:** doesn't fix bone positions by itself.

---

## How to verify the fix

Run the diagnostic script from HoloRoll's `scripts/` folder:

```powershell
python diagnose_glb.py path/to/new_export.glb
```

In the output, check the `[TOP DEFORMATION joint world vs bbox]` block:

```
TOP DEFORMATION joint world vs bbox:
  delta to MESH LOCAL centre:        ( ?, ?, ? )
  delta to bbox after MESH-NODE-WORLD:( ?, ?, ? )
```

**Before fix** (test.glb):
```
  delta to MESH LOCAL centre:        ( 0.843, -1.272, -2.445)
  delta to bbox after MESH-NODE-WORLD:( 0.843, -1.527,  2.081)
```

**Target after fix**: at least one of the two delta lines should have
all components < 0.5 metres for the heaviest-weighted joint. That
indicates the joint is positioned ON the mesh body.

Also verify the validator output:

```powershell
# Use Khronos glTF Validator (web): https://github.khronos.org/glTF-Validator/
```

Should ideally have **zero `NODE_SKINNED_MESH_NON_ROOT` warnings** —
fix by making the mesh node a root (no parent), and applying any axis
conversion as the mesh node's own TRS rather than via a parent chain.

## Reference

- HoloRoll diagnostic script: `scripts/diagnose_glb.py`.
- glTF 2.0 spec, Skins section:
  https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#skins
- Khronos glTF Validator messages list:
  https://github.com/KhronosGroup/glTF-Validator/blob/main/ISSUES.md
- Blender's glTF importer (for "what canonical viewers DON'T do"):
  https://github.com/KhronosGroup/glTF-Blender-IO

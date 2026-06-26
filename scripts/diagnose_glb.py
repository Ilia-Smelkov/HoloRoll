#!/usr/bin/env python3
"""
Standalone GLB diagnostic for HoloRoll's "skeleton offset from mesh" bug.

Parses the .glb file without any external dependencies (just stdlib),
extracts:
  - Scene graph + node hierarchy.
  - Mesh node + its world transform.
  - Skin info (joints, IBM matrices, skeleton root).
  - Joint world positions at rest pose (frame 0 sampled from
    each node's base TRS — no animation applied).
  - Mesh vertex bbox (in MESH LOCAL space, as stored).
  - The delta between joint world positions and the mesh bbox,
    which is the offset HoloRoll's skeleton viz shows.

Usage:
    python diagnose_glb.py path/to/test.glb

Prints a structured dump; paste back to claude.
"""

import json
import struct
import sys
from pathlib import Path

# ---------- glTF binary parser (header + JSON + BIN chunks) ----------

def parse_glb(path: Path):
    """Read .glb, return (json_dict, bin_data_bytes)."""
    data = path.read_bytes()
    if data[:4] != b"glTF":
        sys.exit(f"not a GLB (magic = {data[:4]!r})")
    version, total_len = struct.unpack_from("<II", data, 4)
    print(f"  GLB version={version}, total={total_len} bytes")
    # Chunk 0 = JSON.
    off = 12
    j_len, j_type = struct.unpack_from("<II", data, off); off += 8
    assert j_type == 0x4E4F534A, f"first chunk not JSON: {j_type:08x}"
    j_bytes = data[off:off + j_len]
    off += j_len
    j_doc = json.loads(j_bytes.rstrip(b"\x00").decode("utf-8"))
    # Chunk 1 = BIN (optional).
    bin_data = b""
    if off < len(data):
        b_len, b_type = struct.unpack_from("<II", data, off); off += 8
        assert b_type == 0x004E4942, f"second chunk not BIN: {b_type:08x}"
        bin_data = data[off:off + b_len]
    return j_doc, bin_data


# ---------- 4x4 column-major matrix helpers (glTF convention) ----------

def mat_identity():
    return [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]


def mat_translate(t):
    m = mat_identity()
    m[12], m[13], m[14] = t[0], t[1], t[2]
    return m


def mat_scale(s):
    return [s[0], 0.0, 0.0, 0.0,
            0.0, s[1], 0.0, 0.0,
            0.0, 0.0, s[2], 0.0,
            0.0, 0.0, 0.0, 1.0]


def mat_from_quat(q):
    """Quaternion (x, y, z, w) → column-major rotation matrix."""
    x, y, z, w = q
    xx, yy, zz = x*x, y*y, z*z
    xy, xz, yz = x*y, x*z, y*z
    wx, wy, wz = w*x, w*y, w*z
    return [
        1.0 - 2.0*(yy+zz), 2.0*(xy+wz),       2.0*(xz-wy),       0.0,
        2.0*(xy-wz),       1.0 - 2.0*(xx+zz), 2.0*(yz+wx),       0.0,
        2.0*(xz+wy),       2.0*(yz-wx),       1.0 - 2.0*(xx+yy), 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]


def mat_mul(a, b):
    """Column-major A * B."""
    out = [0.0] * 16
    for c in range(4):  # column of B
        for r in range(4):  # row
            s = 0.0
            for k in range(4):
                s += a[k * 4 + r] * b[c * 4 + k]
            out[c * 4 + r] = s
    return out


def compose_trs(t, r, s):
    """T * R * S as a 4x4 column-major matrix."""
    return mat_mul(mat_mul(mat_translate(t), mat_from_quat(r)), mat_scale(s))


def mat_transform_point(m, p):
    """4x4 column-major * (px, py, pz, 1) → (x, y, z)."""
    px, py, pz = p
    x = m[0]*px + m[4]*py + m[8]*pz  + m[12]
    y = m[1]*px + m[5]*py + m[9]*pz  + m[13]
    z = m[2]*px + m[6]*py + m[10]*pz + m[14]
    return (x, y, z)


def mat_pretty(m):
    """4 lines of 4 floats. m is column-major; print as rows."""
    return "\n".join(
        "    [ " + " ".join(f"{m[c*4 + r]:9.4f}" for c in range(4)) + " ]"
        for r in range(4)
    )


# ---------- glTF semantics ----------

def node_base_trs(n):
    """Extract (translation, rotation_quat, scale) with defaults."""
    if "matrix" in n:
        # Less common; we don't decompose here, just flag it.
        return None
    t = tuple(n.get("translation", (0.0, 0.0, 0.0)))
    r = tuple(n.get("rotation", (0.0, 0.0, 0.0, 1.0)))
    s = tuple(n.get("scale", (1.0, 1.0, 1.0)))
    return (t, r, s)


def build_parents(nodes):
    parents = [-1] * len(nodes)
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parents[c] = i
    return parents


def node_world(node_idx, nodes, parents):
    """Compose this node's world matrix by walking up parents."""
    m = mat_identity()
    chain = []
    n = node_idx
    while n >= 0:
        chain.append(n)
        n = parents[n]
    for n in reversed(chain):
        trs = node_base_trs(nodes[n])
        if trs is None:
            # node has explicit matrix — use it verbatim.
            mat = nodes[n]["matrix"]
            m = mat_mul(m, list(mat))
        else:
            m = mat_mul(m, compose_trs(*trs))
    return m


def find_mesh_node(nodes):
    """First node with a mesh + skin. Returns (idx, mesh_idx, skin_idx)."""
    for i, n in enumerate(nodes):
        if "mesh" in n and "skin" in n:
            return i, n["mesh"], n["skin"]
    for i, n in enumerate(nodes):
        if "mesh" in n:
            return i, n["mesh"], n.get("skin", -1)
    return -1, -1, -1


def accessor_floats(j_doc, bin_data, accessor_idx, components):
    """Extract floats from an accessor (assuming float32, tightly packed)."""
    acc = j_doc["accessors"][accessor_idx]
    bv = j_doc["bufferViews"][acc["bufferView"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    stride = bv.get("byteStride", components * 4)
    out = []
    for i in range(count):
        base = off + i * stride
        vals = struct.unpack_from(f"<{components}f", bin_data, base)
        out.append(vals)
    return out


def accessor_ints(j_doc, bin_data, accessor_idx, components):
    """Extract integer indices from an accessor.
    Handles UBYTE (5121), USHORT (5123), UINT (5125)."""
    acc = j_doc["accessors"][accessor_idx]
    bv = j_doc["bufferViews"][acc["bufferView"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    ct = acc["componentType"]
    if ct == 5121:
        fmt = "B"; sz = 1
    elif ct == 5123:
        fmt = "H"; sz = 2
    elif ct == 5125:
        fmt = "I"; sz = 4
    else:
        raise ValueError(f"unhandled componentType {ct}")
    stride = bv.get("byteStride", components * sz)
    out = []
    for i in range(count):
        base = off + i * stride
        vals = struct.unpack_from(f"<{components}{fmt}", bin_data, base)
        out.append(vals)
    return out


# ---------- main diag ----------

def main():
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} path/to/test.glb")
        sys.exit(1)
    path = Path(sys.argv[1])
    if not path.exists():
        sys.exit(f"file not found: {path}")
    print(f"== diagnose: {path} ==")
    j_doc, bin_data = parse_glb(path)

    nodes = j_doc.get("nodes", [])
    skins = j_doc.get("skins", [])
    meshes = j_doc.get("meshes", [])

    print(f"  nodes={len(nodes)} meshes={len(meshes)} skins={len(skins)}")
    print(f"  animations={len(j_doc.get('animations', []))}")

    parents = build_parents(nodes)

    # --- find skinned mesh node ---
    mesh_node_idx, mesh_idx, skin_idx = find_mesh_node(nodes)
    if mesh_node_idx < 0:
        sys.exit("no mesh node found")
    mesh_node = nodes[mesh_node_idx]
    print(f"\n[mesh node {mesh_node_idx}] name={mesh_node.get('name', '<unnamed>')!r}")
    print(f"  mesh={mesh_idx}, skin={skin_idx}")
    print(f"  local TRS: translation={mesh_node.get('translation')}, "
          f"rotation={mesh_node.get('rotation')}, scale={mesh_node.get('scale')}")
    print(f"  parent={parents[mesh_node_idx]}")

    mesh_node_world = node_world(mesh_node_idx, nodes, parents)
    print(f"  mesh node WORLD matrix:")
    print(mat_pretty(mesh_node_world))
    print(f"  mesh node world translation: "
          f"({mesh_node_world[12]:.4f}, {mesh_node_world[13]:.4f}, {mesh_node_world[14]:.4f})")

    # --- skin info ---
    if skin_idx >= 0:
        skin = skins[skin_idx]
        joints = skin["joints"]
        sk_root = skin.get("skeleton", -1)
        print(f"\n[skin {skin_idx}] name={skin.get('name', '<unnamed>')!r}")
        print(f"  joint count: {len(joints)}")
        print(f"  skin.skeleton: {sk_root}" +
              (f" ({nodes[sk_root].get('name', '?')!r})" if sk_root >= 0 else " (UNSET)"))

        if sk_root >= 0:
            sk_world = node_world(sk_root, nodes, parents)
            print(f"  skin.skeleton world translation: "
                  f"({sk_world[12]:.4f}, {sk_world[13]:.4f}, {sk_world[14]:.4f})")

        # Joint 0 (typically the root joint).
        j0 = joints[0]
        j0_world = node_world(j0, nodes, parents)
        print(f"\n[joint 0 = node {j0}] name={nodes[j0].get('name', '?')!r}")
        print(f"  parent chain: ", end="")
        n = j0
        chain = []
        while n >= 0:
            chain.append(f"{n}:{nodes[n].get('name', '?')}")
            n = parents[n]
        print(" -> ".join(chain))
        print(f"  world translation: "
              f"({j0_world[12]:.4f}, {j0_world[13]:.4f}, {j0_world[14]:.4f})")

        # --- IBM[0] ---
        if "inverseBindMatrices" in skin:
            ibms = accessor_floats(j_doc, bin_data, skin["inverseBindMatrices"], 16)
            if ibms:
                ibm0 = list(ibms[0])
                print(f"\n[IBM 0] (inverse bind matrix of joint 0):")
                print(mat_pretty(ibm0))
                print(f"  IBM[0] translation column: "
                      f"({ibm0[12]:.4f}, {ibm0[13]:.4f}, {ibm0[14]:.4f})")
                # Bind pose joint position = inverse(IBM) translation.
                # IBM is inverse_bind, so bind matrix translation = -R^T * IBM.t.
                # For sanity, the BIND position should equal joint world (which we computed).
                # If they differ, the IBM doesn't encode the actual bind pose we walked.
                # We just print numbers — the user compares.

    # --- mesh vertex bbox ---
    mesh = meshes[mesh_idx]
    prim = mesh["primitives"][0]
    pos_acc_idx = prim["attributes"]["POSITION"]
    pos = accessor_floats(j_doc, bin_data, pos_acc_idx, 3)
    xs = [p[0] for p in pos]
    ys = [p[1] for p in pos]
    zs = [p[2] for p in pos]
    bb_min = (min(xs), min(ys), min(zs))
    bb_max = (max(xs), max(ys), max(zs))
    bb_centre = ((bb_min[0]+bb_max[0])/2, (bb_min[1]+bb_max[1])/2, (bb_min[2]+bb_max[2])/2)
    print(f"\n[mesh primitive 0] vertices={len(pos)}")
    print(f"  bbox MESH LOCAL min: ({bb_min[0]:.4f}, {bb_min[1]:.4f}, {bb_min[2]:.4f})")
    print(f"  bbox MESH LOCAL max: ({bb_max[0]:.4f}, {bb_max[1]:.4f}, {bb_max[2]:.4f})")
    print(f"  bbox MESH LOCAL centre: "
          f"({bb_centre[0]:.4f}, {bb_centre[1]:.4f}, {bb_centre[2]:.4f})")

    # Vertex centre transformed by mesh node world transform.
    centre_world = mat_transform_point(mesh_node_world, bb_centre)
    print(f"  bbox centre after MESH NODE WORLD: "
          f"({centre_world[0]:.4f}, {centre_world[1]:.4f}, {centre_world[2]:.4f})")

    # --- delta ---
    if skin_idx >= 0:
        j0 = j_doc["skins"][skin_idx]["joints"][0]
        j0_world = node_world(j0, nodes, parents)
        delta_no_meshnode = (
            j0_world[12] - bb_centre[0],
            j0_world[13] - bb_centre[1],
            j0_world[14] - bb_centre[2],
        )
        delta_with_meshnode = (
            j0_world[12] - centre_world[0],
            j0_world[13] - centre_world[1],
            j0_world[14] - centre_world[2],
        )
        print(f"\n[DELTAS]")
        print(f"  joint0_world - bbox_centre (mesh local) = "
              f"({delta_no_meshnode[0]:.4f}, {delta_no_meshnode[1]:.4f}, {delta_no_meshnode[2]:.4f})")
        print(f"  joint0_world - bbox_centre (after mesh node world) = "
              f"({delta_with_meshnode[0]:.4f}, {delta_with_meshnode[1]:.4f}, {delta_with_meshnode[2]:.4f})")
        print()
        print("  If 'mesh local' delta is large but 'after mesh node world' is ~0:")
        print("    → fix: apply meshNodeWorld^-1 to joint matrices (case A).")
        print("  If both deltas are similar and large:")
        print("    → joints live in a different reference frame; check skin.skeleton (case B).")
        print("  If both deltas are near zero:")
        print("    → bug is elsewhere (rendering / rotation order / pivot).")

    # ---- joint-weight diagnostic (new in v2) ----
    if skin_idx >= 0 and "attributes" in prim:
        attrs = prim["attributes"]
        if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
            print(f"\n[skinning weight diagnostic]")
            joints_per_vert = accessor_ints(j_doc, bin_data, attrs["JOINTS_0"], 4)
            weights_per_vert = accessor_floats(j_doc, bin_data, attrs["WEIGHTS_0"], 4)
            joint_count = len(j_doc["skins"][skin_idx]["joints"])
            wsum = [0.0] * joint_count
            for jv, wv in zip(joints_per_vert, weights_per_vert):
                for k in range(4):
                    if jv[k] < joint_count:
                        wsum[jv[k]] += wv[k]
            # Top 8 deformation joints by weight.
            ranked = sorted(range(joint_count), key=lambda i: -wsum[i])
            joints_arr = j_doc["skins"][skin_idx]["joints"]
            print(f"  total joints: {joint_count}")
            print(f"  joints with nonzero weight: "
                  f"{sum(1 for w in wsum if w > 1e-6)}")
            print(f"  TOP 8 deformation joints (by total weight):")
            for rank, j_idx in enumerate(ranked[:8]):
                node_idx = joints_arr[j_idx]
                jpos = node_world(node_idx, nodes, parents)
                name = nodes[node_idx].get("name", "?")
                print(f"    #{rank+1} joint[{j_idx:3d}] = node[{node_idx:3d}]"
                      f" '{name}' weight_sum={wsum[j_idx]:8.2f}"
                      f"  world=({jpos[12]:7.3f},{jpos[13]:7.3f},{jpos[14]:7.3f})")
            # Bottom of the list — confirm there are zero-weight (control) joints.
            zero_count = sum(1 for w in wsum if w <= 1e-6)
            if zero_count > 0:
                print(f"  {zero_count} joints have ZERO weight (control/rig nodes).")
                # Show first 3 zero-weight joints with positions.
                zeros = [i for i in range(joint_count) if wsum[i] <= 1e-6][:3]
                for j_idx in zeros:
                    node_idx = joints_arr[j_idx]
                    jpos = node_world(node_idx, nodes, parents)
                    name = nodes[node_idx].get("name", "?")
                    print(f"    zero-weight joint[{j_idx:3d}] '{name}'"
                          f"  world=({jpos[12]:7.3f},{jpos[13]:7.3f},{jpos[14]:7.3f})")

            # Compare top deformation joints to mesh bbox.
            if ranked and wsum[ranked[0]] > 1e-6:
                top = ranked[0]
                node_idx = joints_arr[top]
                jpos = node_world(node_idx, nodes, parents)
                delta_local = (jpos[12] - bb_centre[0],
                               jpos[13] - bb_centre[1],
                               jpos[14] - bb_centre[2])
                delta_world = (jpos[12] - centre_world[0],
                               jpos[13] - centre_world[1],
                               jpos[14] - centre_world[2])
                print(f"\n  TOP DEFORMATION joint world vs bbox:")
                print(f"    delta to MESH LOCAL centre:        "
                      f"({delta_local[0]:7.3f},{delta_local[1]:7.3f},{delta_local[2]:7.3f})")
                print(f"    delta to bbox after MESH-NODE-WORLD:"
                      f"({delta_world[0]:7.3f},{delta_world[1]:7.3f},{delta_world[2]:7.3f})")
                print("    (small delta = joint right ON the mesh body — expected for deformation)")
                print("    (large delta even on TOP joint = something deeper is wrong)")

    # ---- simulate HoloRoll's bake-pose skinning to see where the mesh actually lands ----
    if skin_idx >= 0 and "attributes" in prim:
        attrs = prim["attributes"]
        if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
            print(f"\n[simulating HoloRoll's bake-pose skinning]")
            print(f"  formula: vertex_baked = joint_world * IBM * vertex_mesh_local")
            print(f"  this is exactly what glb_loader.cpp computes per frame.")
            print()
            joints_per_vert = accessor_ints(j_doc, bin_data, attrs["JOINTS_0"], 4)
            weights_per_vert = accessor_floats(j_doc, bin_data, attrs["WEIGHTS_0"], 4)
            joints_arr = j_doc["skins"][skin_idx]["joints"]

            # Pre-compute joint_world * IBM for every joint (at bind pose).
            ibm_list = accessor_floats(j_doc, bin_data, j_doc["skins"][skin_idx]["inverseBindMatrices"], 16)
            skin_mats = []
            for j_idx in range(len(joints_arr)):
                jw = node_world(joints_arr[j_idx], nodes, parents)
                ib = list(ibm_list[j_idx])
                skin_mats.append(mat_mul(jw, ib))

            # Bake each vertex.
            baked = []
            for v_idx in range(len(pos)):
                jv = joints_per_vert[v_idx]
                wv = weights_per_vert[v_idx]
                p = pos[v_idx]
                acc = [0.0, 0.0, 0.0]
                wsum = 0.0
                for k in range(4):
                    w = wv[k]
                    if w <= 0.0:
                        continue
                    j = jv[k]
                    if j >= len(skin_mats):
                        continue
                    m = skin_mats[j]
                    # Apply m to (p, 1).
                    x = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12]
                    y = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13]
                    z = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14]
                    acc[0] += w * x
                    acc[1] += w * y
                    acc[2] += w * z
                    wsum += w
                if wsum > 0.0:
                    baked.append((acc[0]/wsum, acc[1]/wsum, acc[2]/wsum))

            if baked:
                bx = [b[0] for b in baked]
                by = [b[1] for b in baked]
                bz = [b[2] for b in baked]
                bk_min = (min(bx), min(by), min(bz))
                bk_max = (max(bx), max(by), max(bz))
                bk_centre = ((bk_min[0]+bk_max[0])/2,
                             (bk_min[1]+bk_max[1])/2,
                             (bk_min[2]+bk_max[2])/2)
                print(f"  baked vertex bbox min: ({bk_min[0]:7.3f},{bk_min[1]:7.3f},{bk_min[2]:7.3f})")
                print(f"  baked vertex bbox max: ({bk_max[0]:7.3f},{bk_max[1]:7.3f},{bk_max[2]:7.3f})")
                print(f"  baked vertex bbox centre: "
                      f"({bk_centre[0]:7.3f},{bk_centre[1]:7.3f},{bk_centre[2]:7.3f})")
                # Top deformation joint position for comparison.
                if "WEIGHTS_0" in attrs:
                    # Reuse wsum computed earlier (was: per-joint total weight).
                    pass
                # Compare bk_centre to:
                #   joint0_world (which is what the skeleton viz dots are at).
                j0_world_pos = node_world(j_doc["skins"][skin_idx]["joints"][0], nodes, parents)
                d = (bk_centre[0] - j0_world_pos[12],
                     bk_centre[1] - j0_world_pos[13],
                     bk_centre[2] - j0_world_pos[14])
                print(f"\n  baked centre - joint0_world = ({d[0]:7.3f},{d[1]:7.3f},{d[2]:7.3f})")
                print()
                print("  Interpretation:")
                print("  - If baked centre ≈ joint cluster (small delta):")
                print("    mesh + joints land together in HoloRoll, alignment is right.")
                print("    bug then is elsewhere (autoPivot / camera / etc).")
                print("  - If baked centre ≈ mesh-local centre  (-0.90, 2.14, 2.39):")
                print("    IBM is PURE inverse (no meshNodeWorld baked). Our code puts")
                print("    mesh at mesh-LOCAL coords but joints at SCENE-WORLD coords →")
                print("    they live in different frames, hence offset. Fix: multiply")
                print("    vertex_mesh_local by meshNodeWorld before skinning.")
                print("  - If baked centre ≈ mesh-after-world  (-0.90, 2.39, -2.14):")
                print("    IBM HAS meshNodeWorld baked, mesh in scene world. Then joints")
                print("    being at a separate cluster means rig itself is far from mesh")
                print("    (which would be unusual given they're top-weighted).")

    print(f"\n== done ==")


if __name__ == "__main__":
    main()

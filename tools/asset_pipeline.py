#!/usr/bin/env python3
"""
MassRTS Asset Pipeline — Offline FBX/glTF → Engine Format Converter

Converts 3D models (with optional skeleton + animations) into a fast binary
format the engine loads directly at runtime. No Assimp dependency needed at
runtime — only this offline tool uses it.

USAGE:
    pip install numpy pyassimp   (or: pip install numpy pygltflib trimesh)
    python asset_pipeline.py knight.fbx --anims idle.fbx walk.fbx attack.fbx death.fbx

OUTPUT (per model):
    assets/models/<name>.mesh     Binary mesh (vertices + indices + bone weights)
    assets/models/<name>.anim     Animation texture (raw RGBA32F)
    assets/models/<name>.meta     JSON metadata (bone count, clip offsets, frame counts)

FORMATS SUPPORTED:
    .fbx, .gltf, .glb, .obj, .dae (anything Assimp handles)
"""

import sys
import os
import json
import struct
import argparse
import numpy as np

try:
    import pyassimp
    from pyassimp import load as ai_load, release as ai_release
    HAS_ASSIMP = True
except ImportError:
    HAS_ASSIMP = False
    print("[WARN] pyassimp not installed. Install with: pip install pyassimp")
    print("       You also need the Assimp DLL/so on your PATH.")

# ============================================================================
# Binary format definitions
# ============================================================================
# .mesh format:
#   Header: magic(4) version(4) vert_count(4) index_count(4) bone_count(4) has_bones(4)
#   Vertices: [pos.xyz, norm.xyz, uv.xy, bone_ids.xyzw(uint8), bone_weights.xyzw(float)] × vert_count
#   Indices: uint32 × index_count
#
# .anim format (Animation Texture):
#   Raw RGBA32F texture data, width = bone_count * 4, height = total_frames
#   Each bone occupies 4 pixels (= one 4x4 matrix row-by-row in RGBA)
#   Clips are stacked vertically: [idle_frames | walk_frames | attack_frames | death_frames]
#
# .meta format (JSON):
#   { "bones": N, "clips": { "idle": {"start":0,"frames":30,"fps":24}, ... } }

MESH_MAGIC = b'MRTS'
MESH_VERSION = 1
VERT_STRIDE = 3+3+2+4+4  # pos(3) + norm(3) + uv(2) + bone_ids_as_float(4) + weights(4) = 16 floats

# ============================================================================
# Mesh extraction
# ============================================================================

def extract_mesh(scene):
    """Extract first mesh from scene, return (vertices, indices, bones_map)."""
    mesh = scene.meshes[0]
    verts = []
    
    has_bones = len(mesh.bones) > 0
    bone_map = {}  # bone_name -> index
    
    if has_bones:
        for bi, bone in enumerate(mesh.bones):
            bone_map[bone.name] = bi
    
    # Build per-vertex bone weights
    num_verts = len(mesh.vertices)
    vert_bones = np.zeros((num_verts, 4), dtype=np.int32)
    vert_weights = np.zeros((num_verts, 4), dtype=np.float32)
    
    if has_bones:
        for bi, bone in enumerate(mesh.bones):
            for weight in bone.weights:
                vid = weight.vertexid
                w = weight.weight
                # Find empty slot (weight == 0)
                for s in range(4):
                    if vert_weights[vid, s] == 0.0:
                        vert_bones[vid, s] = bi
                        vert_weights[vid, s] = w
                        break
        # Normalize weights
        sums = vert_weights.sum(axis=1, keepdims=True)
        sums[sums == 0] = 1.0
        vert_weights /= sums
    
    # Build vertex buffer
    has_uvs = mesh.texturecoords is not None and len(mesh.texturecoords) > 0
    for vi in range(num_verts):
        pos = mesh.vertices[vi]
        norm = mesh.normals[vi] if mesh.normals is not None else [0,1,0]
        uv = mesh.texturecoords[0][vi][:2] if has_uvs else [0, 0]
        bi = vert_bones[vi]
        bw = vert_weights[vi]
        verts.extend([pos[0], pos[1], pos[2],
                      norm[0], norm[1], norm[2],
                      uv[0], uv[1],
                      float(bi[0]), float(bi[1]), float(bi[2]), float(bi[3]),
                      bw[0], bw[1], bw[2], bw[3]])
    
    # Build index buffer
    indices = []
    for face in mesh.faces:
        if len(face.indices) == 3:
            indices.extend(face.indices)
    
    return np.array(verts, dtype=np.float32), np.array(indices, dtype=np.uint32), bone_map, has_bones


# ============================================================================
# Animation baking (Animation Texture)
# ============================================================================

def bake_animation_texture(scene, bone_map, clip_name="default"):
    """Bake all animations in scene into a texture. Returns (texture_data, clip_info)."""
    bone_count = len(bone_map)
    if bone_count == 0:
        return None, {}
    
    clips = {}
    all_frames = []
    
    for ai, anim in enumerate(scene.animations):
        name = clip_name if len(scene.animations) == 1 else anim.name or f"clip_{ai}"
        fps = anim.tickspersecond if anim.tickspersecond > 0 else 24.0
        duration = anim.duration / fps
        num_frames = max(1, int(duration * 24))  # bake at 24fps
        
        clip_start = len(all_frames)
        
        for fi in range(num_frames):
            t = fi / 24.0
            frame_matrices = np.eye(4, dtype=np.float32)[np.newaxis].repeat(bone_count, axis=0)
            
            for channel in anim.channels:
                bone_name = channel.nodename
                if bone_name not in bone_map:
                    continue
                bi = bone_map[bone_name]
                
                # Sample position/rotation/scale at time t
                mat = sample_channel(channel, t * fps)
                frame_matrices[bi] = mat
            
            all_frames.append(frame_matrices)
        
        clips[name] = {"start": clip_start, "frames": num_frames, "fps": 24}
    
    if not all_frames:
        return None, {}
    
    # Pack into texture: width = bone_count * 4 pixels, height = total_frames
    # Each bone = 4 RGBA pixels = 16 floats = one 4x4 matrix
    total_frames = len(all_frames)
    tex_width = bone_count * 4
    tex_data = np.zeros((total_frames, tex_width, 4), dtype=np.float32)
    
    for fi, frame_mats in enumerate(all_frames):
        for bi in range(bone_count):
            mat = frame_mats[bi]  # 4x4
            for row in range(4):
                px = bi * 4 + row
                tex_data[fi, px, :] = mat[row, :]
    
    return tex_data, clips


def sample_channel(channel, tick):
    """Sample a single animation channel at given tick, return 4x4 matrix."""
    # Position
    pos = np.zeros(3)
    if len(channel.positionkeys) > 0:
        pos = interpolate_keys_vec3(channel.positionkeys, tick)
    
    # Rotation (quaternion)
    quat = np.array([1, 0, 0, 0], dtype=np.float64)  # w,x,y,z
    if len(channel.rotationkeys) > 0:
        quat = interpolate_keys_quat(channel.rotationkeys, tick)
    
    # Scale
    scl = np.ones(3)
    if len(channel.scalingkeys) > 0:
        scl = interpolate_keys_vec3(channel.scalingkeys, tick)
    
    mat = quat_to_mat4(quat)
    mat[0, :3] *= scl[0]
    mat[1, :3] *= scl[1]
    mat[2, :3] *= scl[2]
    mat[0, 3] = pos[0]
    mat[1, 3] = pos[1]
    mat[2, 3] = pos[2]
    return mat.astype(np.float32)


def interpolate_keys_vec3(keys, tick):
    """Linear interpolate vec3 keys at tick."""
    if len(keys) == 1:
        return np.array(keys[0].value, dtype=np.float64)
    for i in range(len(keys) - 1):
        if tick <= keys[i+1].time:
            t = (tick - keys[i].time) / max(keys[i+1].time - keys[i].time, 1e-6)
            t = np.clip(t, 0, 1)
            a = np.array(keys[i].value, dtype=np.float64)
            b = np.array(keys[i+1].value, dtype=np.float64)
            return a + (b - a) * t
    return np.array(keys[-1].value, dtype=np.float64)


def interpolate_keys_quat(keys, tick):
    """Slerp quaternion keys at tick. Returns [w,x,y,z]."""
    if len(keys) == 1:
        q = keys[0].value
        return np.array([q.w, q.x, q.y, q.z], dtype=np.float64)
    for i in range(len(keys) - 1):
        if tick <= keys[i+1].time:
            t = (tick - keys[i].time) / max(keys[i+1].time - keys[i].time, 1e-6)
            t = np.clip(t, 0, 1)
            q0 = keys[i].value
            q1 = keys[i+1].value
            a = np.array([q0.w, q0.x, q0.y, q0.z], dtype=np.float64)
            b = np.array([q1.w, q1.x, q1.y, q1.z], dtype=np.float64)
            return slerp(a, b, t)
    q = keys[-1].value
    return np.array([q.w, q.x, q.y, q.z], dtype=np.float64)


def slerp(q0, q1, t):
    dot = np.dot(q0, q1)
    if dot < 0:
        q1 = -q1; dot = -dot
    if dot > 0.9995:
        return q0 + t * (q1 - q0)
    theta = np.arccos(np.clip(dot, -1, 1))
    sin_theta = np.sin(theta)
    return (np.sin((1-t)*theta) * q0 + np.sin(t*theta) * q1) / sin_theta


def quat_to_mat4(q):
    """Quaternion [w,x,y,z] to 4x4 rotation matrix."""
    w, x, y, z = q
    m = np.eye(4, dtype=np.float64)
    m[0,0] = 1 - 2*(y*y + z*z); m[0,1] = 2*(x*y - w*z);     m[0,2] = 2*(x*z + w*y)
    m[1,0] = 2*(x*y + w*z);     m[1,1] = 1 - 2*(x*x + z*z); m[1,2] = 2*(y*z - w*x)
    m[2,0] = 2*(x*z - w*y);     m[2,1] = 2*(y*z + w*x);     m[2,2] = 1 - 2*(x*x + y*y)
    return m


# ============================================================================
# File output
# ============================================================================

def write_mesh(path, verts, indices, bone_count, has_bones):
    with open(path, 'wb') as f:
        f.write(MESH_MAGIC)
        f.write(struct.pack('<I', MESH_VERSION))
        f.write(struct.pack('<I', len(verts) // VERT_STRIDE))
        f.write(struct.pack('<I', len(indices)))
        f.write(struct.pack('<I', bone_count))
        f.write(struct.pack('<I', 1 if has_bones else 0))
        f.write(verts.tobytes())
        f.write(indices.tobytes())
    print(f"  wrote {path} ({os.path.getsize(path)} bytes, {len(verts)//VERT_STRIDE} verts, {len(indices)} tris)")


def write_anim_texture(path, tex_data):
    with open(path, 'wb') as f:
        h, w, _ = tex_data.shape
        f.write(struct.pack('<II', w, h))  # width, height
        f.write(tex_data.tobytes())
    print(f"  wrote {path} ({os.path.getsize(path)} bytes, {tex_data.shape[1]}x{tex_data.shape[0]})")


def write_meta(path, bone_count, clips):
    meta = {"bones": bone_count, "clips": clips}
    with open(path, 'w') as f:
        json.dump(meta, f, indent=2)
    print(f"  wrote {path}")


# ============================================================================
# Main
# ============================================================================

def convert(model_path, anim_paths=None, output_name=None):
    if not HAS_ASSIMP:
        print("ERROR: pyassimp required. Install: pip install pyassimp")
        sys.exit(1)
    
    if output_name is None:
        output_name = os.path.splitext(os.path.basename(model_path))[0]
    
    out_dir = os.path.join("assets", "models")
    os.makedirs(out_dir, exist_ok=True)
    
    print(f"[pipeline] Loading {model_path}...")
    scene = ai_load(model_path,
                    processing=pyassimp.postprocess.aiProcess_Triangulate |
                               pyassimp.postprocess.aiProcess_LimitBoneWeights |
                               pyassimp.postprocess.aiProcess_GenNormals)
    
    verts, indices, bone_map, has_bones = extract_mesh(scene)
    write_mesh(os.path.join(out_dir, f"{output_name}.mesh"), verts, indices, len(bone_map), has_bones)
    
    # Bake animations
    all_clips = {}
    all_tex_frames = []
    
    # Animations from the model file itself
    if scene.animations:
        tex, clips = bake_animation_texture(scene, bone_map)
        if tex is not None:
            all_tex_frames.append(tex)
            all_clips.update(clips)
    
    ai_release(scene)
    
    # Additional animation files (separate FBX per clip)
    if anim_paths:
        for apath in anim_paths:
            clip_name = os.path.splitext(os.path.basename(apath))[0]
            print(f"  baking clip '{clip_name}' from {apath}...")
            ascene = ai_load(apath, processing=pyassimp.postprocess.aiProcess_Triangulate)
            if ascene.animations:
                # Offset starts by what we already have
                offset = sum(t.shape[0] for t in all_tex_frames)
                tex, clips = bake_animation_texture(ascene, bone_map, clip_name)
                if tex is not None:
                    # Adjust clip starts
                    for k in clips:
                        clips[k]["start"] += offset
                    all_tex_frames.append(tex)
                    all_clips.update(clips)
            ai_release(ascene)
    
    # Write animation texture + meta
    if all_tex_frames:
        combined = np.concatenate(all_tex_frames, axis=0)
        write_anim_texture(os.path.join(out_dir, f"{output_name}.anim"), combined)
        write_meta(os.path.join(out_dir, f"{output_name}.meta"), len(bone_map), all_clips)
    elif has_bones:
        # Has skeleton but no animations — write identity bind pose
        print("  [WARN] Model has bones but no animations found")
        write_meta(os.path.join(out_dir, f"{output_name}.meta"), len(bone_map), {})
    
    print(f"[pipeline] Done: {output_name} ({'skinned' if has_bones else 'static'}, {len(all_clips)} clips)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MassRTS Asset Pipeline: FBX/glTF → engine format")
    parser.add_argument("model", help="Path to model file (.fbx/.gltf/.obj)")
    parser.add_argument("--anims", nargs="*", help="Additional animation files (one per clip)")
    parser.add_argument("--name", help="Output name (default: filename without extension)")
    args = parser.parse_args()
    convert(args.model, args.anims, args.name)

"""Export skeletal ("maya") animation for a mode's NPCs and spot animations.

Newer models carry per-vertex bone weights and newer sequences animate bones instead of vertex
groups (rc_cache/skeletal.py). The shared models already hold these models' vertices in cache
order; this exporter adds what playback needs:

  rigs       per NPC definition (its models merged in definition order) or spotanim model: each
             vertex's (bone slot, weight 0..255) pairs
  sequences  per client tick (20 ms) of [anim_maya_start, anim_maya_end), each used bone's skin
             matrix (inverse bind-pose world times animated world) in model space

An NPC is skeletal when its models carry bone weights and its stand sequence is skeletal; every
sequence on its skeleton is exported. A spotanim is skeletal when its model and sequence are.

File (little-endian): "SKL1", version, skeleton, rig and sequence counts; per skeleton its id,
slot count and the bone id of each slot; per rig its kind (0 NPC definition, 1 model), id,
skeleton index, vertex count, pair count, a pair count per vertex and the (slot, weight) pairs;
per sequence its id, skeleton index, frame count, walk and run flags and frame x slot 3x4 float
matrices (rows of the linear part, then the translation) for row vectors v' = v M.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rc_cache import (  # noqa: E402
    CONFIG_NPC,
    CONFIG_SEQUENCE,
    CONFIG_SPOTANIM,
    RcCacheStore,
    decode_npc_definition,
    decode_sequence_definition,
    read_config_group,
)
from rc_cache.definitions import decode_spotanim_definition  # noqa: E402
from rc_cache.models import load_model_bytes  # noqa: E402
from rc_cache.skeletal import (  # noqa: E402
    INDEX_KEYFRAMES,
    INDEX_SKELETONS,
    decode_keyframes,
    decode_skeleton,
    decode_vertex_bones,
    skin_matrix_frames,
)

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
MAGIC = b"SKL1"
VERSION = 1
RIG_NPC, RIG_MODEL = 0, 1
# Skin space has the model's y and z negated: conjugating by this flip gives model-space matrices.
FLIP = np.diag([1.0, -1.0, -1.0, 1.0])


@dataclass
class Rig:
    kind: int
    id: int
    skeleton: int
    vertex_bones: list          # per vertex [(bone, weight)]
    sequences: dict = field(default_factory=dict)   # sequence id -> SequenceDef


def _keyframes(store: RcCacheStore, maya_id: int) -> bytes:
    files = store.read_group(INDEX_KEYFRAMES, maya_id >> 16)
    return files[0] if 0 in files else next(iter(files.values()))


def _model_bones(store: RcCacheStore, model_ids: list[int]) -> list | None:
    """Per-vertex bones of models merged in order, or None when one has no bone weights."""
    out = []
    for model_id in model_ids:
        data = load_model_bytes(store, model_id)
        bones = decode_vertex_bones(data) if data else None
        if bones is None:
            return None
        out += bones
    return out


def collect(store: RcCacheStore, npc_ids: list[int], spotanim_ids: list[int]) -> list[Rig]:
    seq_bytes = read_config_group(store, CONFIG_SEQUENCE)
    seqs = {}
    for seq_id, data in seq_bytes.items():
        d = decode_sequence_definition(seq_id, data)
        if d.anim_maya_id >= 0:
            seqs[seq_id] = d
    skeleton_of = {}

    def skeleton(seq_id: int) -> int:
        d = seqs.get(seq_id)
        if d is None:
            return -1
        if seq_id not in skeleton_of:
            data = _keyframes(store, d.anim_maya_id)
            skeleton_of[seq_id] = (data[1] << 8) | data[2]
        return skeleton_of[seq_id]

    rigs = []
    npc_bytes = read_config_group(store, CONFIG_NPC)
    for npc_id in npc_ids:
        if npc_id not in npc_bytes:
            continue
        npc = decode_npc_definition(npc_id, npc_bytes[npc_id])
        skel = skeleton(npc.stand_anim)
        bones = _model_bones(store, npc.models) if skel >= 0 else None
        if bones is not None:
            rigs.append(Rig(RIG_NPC, npc_id, skel, bones))
    if any(r.kind == RIG_NPC for r in rigs):
        for seq_id in sorted(seqs):    # every sequence on an NPC rig's skeleton
            for rig in rigs:
                if rig.kind == RIG_NPC and skeleton(seq_id) == rig.skeleton:
                    rig.sequences[seq_id] = seqs[seq_id]
    spot_bytes = read_config_group(store, CONFIG_SPOTANIM)
    for spot_id in spotanim_ids:
        if spot_id not in spot_bytes:
            continue
        spot = decode_spotanim_definition(spot_id, spot_bytes[spot_id])
        skel = skeleton(spot.animation_id)
        bones = _model_bones(store, [spot.model_id]) if skel >= 0 else None
        if bones is None:
            continue
        rig = next((r for r in rigs if r.kind == RIG_MODEL and r.id == spot.model_id), None)
        if rig is None:
            rig = Rig(RIG_MODEL, spot.model_id, skel, bones)
            rigs.append(rig)
        rig.sequences[spot.animation_id] = seqs[spot.animation_id]
    return rigs


def write(store: RcCacheStore, rigs: list[Rig], path: Path) -> dict:
    skeletons = sorted({r.skeleton for r in rigs})
    slots = {s: sorted({b for r in rigs if r.skeleton == s for pairs in r.vertex_bones for b, _ in pairs})
             for s in skeletons}
    sequences = {}                              # (seq id, skeleton) -> SequenceDef
    for rig in rigs:
        for seq_id, d in rig.sequences.items():
            sequences[(seq_id, rig.skeleton)] = d
    out = bytearray(MAGIC + struct.pack("<4I", VERSION, len(skeletons), len(rigs), len(sequences)))
    for s in skeletons:
        out += struct.pack("<II", s, len(slots[s]))
        out += struct.pack(f"<{len(slots[s])}H", *slots[s])
        out += bytes(-len(out) % 4)
    for rig in rigs:
        slot_of = {b: i for i, b in enumerate(slots[rig.skeleton])}
        pairs = [(slot_of[b], w) for vb in rig.vertex_bones for b, w in vb]
        out += struct.pack("<B3xIIII", rig.kind, rig.id, skeletons.index(rig.skeleton),
                           len(rig.vertex_bones), len(pairs))
        out += bytes(len(vb) for vb in rig.vertex_bones)
        out += bytes(-len(out) % 4)
        out += b"".join(struct.pack("<HH", s, w) for s, w in pairs)
    frames_total = 0
    bones_of = {s: decode_skeleton(store.read_group(INDEX_SKELETONS, s)[0]) for s in skeletons}
    for (seq_id, s), d in sorted(sequences.items()):
        anim = decode_keyframes(_keyframes(store, d.anim_maya_id))
        ts = np.arange(d.anim_maya_start, max(d.anim_maya_end, d.anim_maya_start + 1), dtype=np.float64)
        frames = skin_matrix_frames(anim, bones_of[s], ts)[:, slots[s]]
        frames = FLIP @ frames @ FLIP
        rows = np.concatenate([frames[:, :, :3, :3].reshape(len(ts), -1, 9),
                               frames[:, :, 3, :3]], axis=2).astype("<f4")
        out += struct.pack("<IIIbb2x", seq_id, skeletons.index(s), len(ts),
                           max(-128, min(127, d.priority)), max(-128, min(127, d.precedence_animating)))
        out += rows.tobytes()
        frames_total += len(ts)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return {"skeletons": len(skeletons), "rigs": len(rigs), "sequences": len(sequences),
            "frames": frames_total, "bytes": len(out)}


def _ids(text: str) -> list[int]:
    return [int(x) for x in text.split(",") if x.strip()]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--npcs", type=_ids, default=[], help="NPC definition ids, comma separated")
    parser.add_argument("--spotanims", type=_ids, default=[], help="spotanim ids, comma separated")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    store = RcCacheStore(args.cache)
    rigs = collect(store, sorted(set(args.npcs)), sorted(set(args.spotanims)))
    stats = write(store, rigs, args.output)
    names = ", ".join(f"{'npc' if r.kind == RIG_NPC else 'model'} {r.id} ({len(r.sequences)} seqs)"
                      for r in rigs) or "none"
    print(f"skeletal: {names}; {stats['frames']} frames, {stats['bytes'] / 1e6:.1f} MB "
          f"-> {args.output}", flush=True)


if __name__ == "__main__":
    main()

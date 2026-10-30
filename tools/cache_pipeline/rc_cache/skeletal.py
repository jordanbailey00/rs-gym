"""OSRS skeletal ("maya") animation: skeletons, keyframe curves and per-frame bone matrices.

Newer sequences carry `anim_maya_id` instead of frame ids. Their keyframes live in index 22 (group
`anim_maya_id >> 16`, file 0) and name a skeleton: a framebase (index 1) whose legacy frame-map
groups are followed by bones. Each bone has a parent and, per bind pose, a 4x4 matrix. A curve
animates one channel of one bone (rotation, translation or scale on x, y or z) over client ticks
(20 ms). Matrices are row-major and act on row vectors (v' = v M; translation in 12..14), in a space
with model y and z negated. A vertex is skinned as the sum over its bones of w/255 * (v B^-1 W),
where B is the bone's bind-pose world matrix and W its animated world matrix.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

INDEX_SKELETONS = 1
INDEX_KEYFRAMES = 22
FLT_MAX = 3.4028234663852886e38


class _Buf:
    def __init__(self, data: bytes, pos: int = 0):
        self.data, self.pos = data, pos

    def u8(self) -> int:
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def i16(self) -> int:
        v = struct.unpack_from(">h", self.data, self.pos)[0]
        self.pos += 2
        return v

    def f32(self) -> float:
        v = struct.unpack_from(">f", self.data, self.pos)[0]
        self.pos += 4
        return v

    def short_smart(self) -> int:
        """Signed smart: one byte for -64..63, else two bytes for -16384..16383."""
        if self.data[self.pos] < 128:
            return self.u8() - 64
        return self.u16() - 0xC000

    def remaining(self) -> int:
        return len(self.data) - self.pos


@dataclass
class Bone:
    parent: int
    bind: list            # per bind pose: 4x4 local matrix (numpy, row-major)
    rotation: list        # per bind pose: default Euler (x, y, z) from the inverse matrix
    translation: list     # per bind pose: default translation (matrix 12..14)
    scale: list           # per bind pose: default scale (row lengths)


def _euler_from(m: np.ndarray) -> tuple[float, float, float]:
    """The client's angles from a matrix's rotation part (row-major indices as in the client)."""
    v = m.reshape(16)
    rx = -math.asin(max(-1.0, min(1.0, v[6])))
    if abs(math.cos(rx)) > 0.005:
        return rx, math.atan2(v[2], v[10]), math.atan2(v[4], v[5])
    ry = math.atan2(v[1], v[0])
    return rx, ry if v[6] < 0 else -ry, 0.0


def decode_skeleton(data: bytes) -> list[Bone] | None:
    """Bones from a framebase, or None when it has no skeleton."""
    b = _Buf(data)
    length = b.u8()
    b.pos += length                       # frame-map group types
    counts = [b.u8() for _ in range(length)]
    b.pos += sum(counts)                  # frame-map group members
    if b.remaining() < 2:
        return None
    count = b.u16()
    if count == 0:
        return None
    poses = b.u8()
    bones = []
    for _ in range(count):
        parent = b.i16()
        bind, rot, trans, scale = [], [], [], []
        for _ in range(poses):
            m = np.array([b.f32() for _ in range(16)], dtype=np.float64).reshape(4, 4)
            b.pos += 12                   # three floats the client keeps but playback does not use
            bind.append(m)
            rot.append(_euler_from(np.linalg.inv(m)))
            trans.append((m[3, 0], m[3, 1], m[3, 2]))
            scale.append(tuple(float(np.linalg.norm(m[r, :3])) for r in range(3)))
        bones.append(Bone(parent, bind, rot, trans, scale))
    return bones


@dataclass
class Key:
    time: int
    value: float
    in_dx: float
    in_dy: float
    out_dx: float
    out_dy: float


@dataclass
class Curve:
    keys: list
    pre: int        # out-of-range modes (0 holds the end value)
    post: int
    bezier: bool    # time handles solved as a Bezier in x; otherwise a Hermite in time

    def value(self, t: float) -> float:
        k = self.keys
        if not k:
            return 0.0
        if t <= k[0].time:
            return k[0].value
        if t >= k[-1].time:
            return k[-1].value
        lo, hi = 0, len(k) - 1
        while hi - lo > 1:              # the segment [k[lo], k[lo + 1]] holding t
            mid = (lo + hi) // 2
            if k[mid].time <= t:
                lo = mid
            else:
                hi = mid
        a, n = k[lo], k[lo + 1]
        if a.out_dx == 0 and a.out_dy == 0:
            return a.value
        if a.out_dx >= FLT_MAX and a.out_dy >= FLT_MAX:   # step
            return n.value if t != a.time else a.value
        x0, y0, x3, y3 = a.time, a.value, n.time, n.value
        x1, y1 = x0 + a.out_dx / 3, y0 + a.out_dy / 3
        x2, y2 = x3 - n.in_dx / 3, y3 - n.in_dy / 3
        dx = x3 - x0
        if not self.bezier:
            m0 = (y1 - y0) / (x1 - x0) if x1 != x0 else 0.0
            m1 = (y3 - y2) / (x3 - x2) if x3 != x2 else 0.0
            dy = y3 - y0
            c2 = (3 * dy - (2 * m0 + m1) * dx) / (dx * dx)
            c3 = ((m0 + m1) * dx - 2 * dy) / (dx * dx * dx)
            d = t - x0
            return y0 + d * (m0 + d * (c2 + d * c3))
        # Bezier in x: find u with X(u) = t (handles kept inside the span), then Y(u).
        x1n = min(1.0, max(0.0, (x1 - x0) / dx))
        x2n = min(1.0, max(0.0, (x2 - x0) / dx))
        s = (t - x0) / dx
        ax, bx, cx = 1 - 3 * x2n + 3 * x1n, 3 * (x2n - 2 * x1n), 3 * x1n
        lo_u, hi_u = 0.0, 1.0
        for _ in range(40):             # X(u) is monotonic for handles inside the span
            u = (lo_u + hi_u) / 2
            if ((ax * u + bx) * u + cx) * u < s:
                lo_u = u
            else:
                hi_u = u
        u = (lo_u + hi_u) / 2
        ay = y3 - 3 * y2 + 3 * y1 - y0
        by = 3 * (y0 - 2 * y1 + y2)
        cy = 3 * (y1 - y0)
        return y0 + u * (cy + u * (by + u * ay))

    def sample(self, ts: np.ndarray) -> np.ndarray:
        """`value` at each of `ts`, vectorized."""
        k = self.keys
        if not k:
            return np.zeros(len(ts))
        times = np.array([x.time for x in k], dtype=np.float64)
        out = np.where(ts <= times[0], k[0].value, k[-1].value).astype(np.float64)
        inside = (ts > times[0]) & (ts < times[-1])
        if not inside.any():
            return out
        t = ts[inside]
        seg = np.searchsorted(times, t, side="right") - 1
        col = lambda name, off=0: np.array([getattr(x, name) for x in k])[seg + off]  # noqa: E731
        x0, y0, odx, ody = col("time"), col("value"), col("out_dx"), col("out_dy")
        x3, y3, idx, idy = col("time", 1), col("value", 1), col("in_dx", 1), col("in_dy", 1)
        x1, y1 = x0 + odx / 3, y0 + ody / 3
        x2, y2 = x3 - idx / 3, y3 - idy / 3
        dx = x3 - x0
        if not self.bezier:
            with np.errstate(divide="ignore", invalid="ignore"):
                m0 = np.where(x1 != x0, (y1 - y0) / (x1 - x0), 0.0)
                m1 = np.where(x3 != x2, (y3 - y2) / (x3 - x2), 0.0)
            dy = y3 - y0
            c2 = (3 * dy - (2 * m0 + m1) * dx) / (dx * dx)
            c3 = ((m0 + m1) * dx - 2 * dy) / (dx * dx * dx)
            d = t - x0
            v = y0 + d * (m0 + d * (c2 + d * c3))
        else:
            x1n = np.clip((x1 - x0) / dx, 0.0, 1.0)
            x2n = np.clip((x2 - x0) / dx, 0.0, 1.0)
            s = (t - x0) / dx
            ax, bx, cx = 1 - 3 * x2n + 3 * x1n, 3 * (x2n - 2 * x1n), 3 * x1n
            lo_u, hi_u = np.zeros_like(s), np.ones_like(s)
            for _ in range(40):
                u = (lo_u + hi_u) / 2
                below = ((ax * u + bx) * u + cx) * u < s
                lo_u, hi_u = np.where(below, u, lo_u), np.where(below, hi_u, u)
            u = (lo_u + hi_u) / 2
            ay = y3 - 3 * y2 + 3 * y1 - y0
            by = 3 * (y0 - 2 * y1 + y2)
            cy = 3 * (y1 - y0)
            v = y0 + u * (cy + u * (by + u * ay))
        flat = (odx == 0) & (ody == 0)
        step = (odx >= FLT_MAX) & (ody >= FLT_MAX)
        v = np.where(step, np.where(t != x0, y3, y0), v)
        out[inside] = np.where(flat, y0, v)
        return out


@dataclass
class SkeletalAnimation:
    skeleton: int
    pose: int                                          # which bind pose the bones use
    bones: dict = field(default_factory=dict)          # bone -> {channel 0..8: Curve}
    other: int = 0                                     # curves on legacy groups (not played)


# Component id -> bone channel: rotation x, y, z, translation x, y, z, scale x, y, z.
_BONE_CHANNEL = {i: i - 1 for i in range(1, 10)}


def decode_keyframes(data: bytes) -> SkeletalAnimation:
    b = _Buf(data)
    b.u8()                                # version
    skeleton = b.u16()
    b.u16()
    b.u16()
    pose = b.u8()
    anim = SkeletalAnimation(skeleton, pose)
    for _ in range(b.u16()):
        target = b.u8()
        index = b.short_smart()
        component = b.u8()
        n = b.u16()
        b.u8()
        pre, post, bezier = b.u8(), b.u8(), b.u8() != 0
        keys = [Key(b.i16(), b.f32(), b.f32(), b.f32(), b.f32(), b.f32()) for _ in range(n)]
        if target == 1 and component in _BONE_CHANNEL:
            anim.bones.setdefault(index, {})[_BONE_CHANNEL[component]] = Curve(keys, pre, post, bezier)
        else:
            anim.other += 1
    if b.remaining():
        raise ValueError(f"keyframes: {b.remaining()} bytes left over")
    return anim


def _quat(axis: tuple[float, float, float], angle: float) -> tuple[float, float, float, float]:
    s, c = math.sin(angle / 2), math.cos(angle / 2)
    return axis[0] * s, axis[1] * s, axis[2] * s, c


def _premultiply(q, r):
    """The client's q.multiply(r): q becomes r * q (Hamilton product)."""
    ax, ay, az, aw = r
    bx, by, bz, bw = q
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def _rotation_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    q = (0.0, 0.0, 0.0, 1.0)
    for axis, angle in (((0, 0, 1), rz), ((1, 0, 0), rx), ((0, 1, 0), ry)):
        q = _premultiply(q, _quat(axis, angle))
    x, y, z, w = q
    m = np.identity(4)
    m[0, 0], m[0, 1], m[0, 2] = x * x + w * w - z * z - y * y, 2 * y * x + 2 * z * w, 2 * x * z - 2 * y * w
    m[1, 0], m[1, 1], m[1, 2] = 2 * y * x - 2 * z * w, y * y + w * w - x * x - z * z, 2 * w * x + 2 * z * y
    m[2, 0], m[2, 1], m[2, 2] = 2 * x * z + 2 * y * w, 2 * z * y - 2 * w * x, z * z + w * w - y * y - x * x
    return m


def skin_matrices(anim: SkeletalAnimation, bones: list[Bone], t: float) -> np.ndarray:
    """Per bone, B^-1 W at client tick t: an (n, 4, 4) array for row-vector skinning."""
    p = anim.pose
    local = []
    for i, bone in enumerate(bones):
        ch = anim.bones.get(i, {})
        rx, ry, rz = (ch[k].value(t) if k in ch else bone.rotation[p][k] for k in range(3))
        sx, sy, sz = (ch[k + 6].value(t) if k + 6 in ch else bone.scale[p][k] for k in range(3))
        tx, ty, tz = (ch[k + 3].value(t) if k + 3 in ch else bone.translation[p][k] for k in range(3))
        m = _rotation_matrix(rx, ry, rz) @ np.diag([sx, sy, sz, 1.0])
        m[3, :3] = (tx, ty, tz)
        local.append(m)
    world, bind = [None] * len(bones), [None] * len(bones)

    def solve(i):
        if world[i] is None:
            par = bones[i].parent
            if 0 <= par < len(bones):
                solve(par)
                world[i] = local[i] @ world[par]
                bind[i] = bones[i].bind[p] @ bind[par]
            else:
                world[i] = local[i]
                bind[i] = bones[i].bind[p]
    for i in range(len(bones)):
        solve(i)
    return np.stack([np.linalg.inv(bind[i]) @ world[i] for i in range(len(bones))])


def _rotation_matrices(rx: np.ndarray, ry: np.ndarray, rz: np.ndarray) -> np.ndarray:
    """`_rotation_matrix` for arrays of angles: an (n, 4, 4) array."""
    n = len(rx)
    q = (np.zeros(n), np.zeros(n), np.zeros(n), np.ones(n))
    for axis, angle in (((0, 0, 1), rz), ((1, 0, 0), rx), ((0, 1, 0), ry)):
        s, c = np.sin(angle / 2), np.cos(angle / 2)
        q = _premultiply(q, (axis[0] * s, axis[1] * s, axis[2] * s, c))
    x, y, z, w = q
    m = np.zeros((n, 4, 4))
    m[:, 0, 0], m[:, 0, 1], m[:, 0, 2] = x * x + w * w - z * z - y * y, 2 * y * x + 2 * z * w, 2 * x * z - 2 * y * w
    m[:, 1, 0], m[:, 1, 1], m[:, 1, 2] = 2 * y * x - 2 * z * w, y * y + w * w - x * x - z * z, 2 * w * x + 2 * z * y
    m[:, 2, 0], m[:, 2, 1], m[:, 2, 2] = 2 * x * z + 2 * y * w, 2 * z * y - 2 * w * x, z * z + w * w - y * y - x * x
    m[:, 3, 3] = 1.0
    return m


def skin_matrix_frames(anim: SkeletalAnimation, bones: list[Bone], ts: np.ndarray) -> np.ndarray:
    """`skin_matrices` at each client tick of `ts`: an (len(ts), bones, 4, 4) array."""
    p, n, nb = anim.pose, len(ts), len(bones)
    local = np.zeros((nb, n, 4, 4))
    for i, bone in enumerate(bones):
        ch = anim.bones.get(i, {})
        defaults = (*bone.rotation[p], *bone.translation[p], *bone.scale[p])
        c = [ch[k].sample(ts) if k in ch else np.full(n, defaults[k]) for k in range(9)]
        m = _rotation_matrices(c[0], c[1], c[2])
        m[:, :3, :3] *= np.stack([c[6], c[7], c[8]], axis=1)[:, None, :]   # R @ diag(scale)
        m[:, 3, :3] = np.stack([c[3], c[4], c[5]], axis=1)
        local[i] = m
    world, bind = [None] * nb, [None] * nb

    def solve(i):
        if world[i] is None:
            par = bones[i].parent
            if 0 <= par < nb:
                solve(par)
                world[i] = local[i] @ world[par]
                bind[i] = bones[i].bind[p] @ bind[par]
            else:
                world[i] = local[i]
                bind[i] = bones[i].bind[p]
    for i in range(nb):
        solve(i)
    return np.stack([np.linalg.inv(bind[i]) @ world[i] for i in range(nb)], axis=1)


def decode_vertex_bones(data: bytes) -> list[list[tuple[int, int]]] | None:
    """Per vertex (bone, weight 0..255) pairs from a type-2 or type-3 model, or None."""
    n = len(data)
    if n >= 26 and data[-2] == 0xFF and data[-1] == 0xFD:      # type 3
        f = n - 26
        verts, faces, texs = struct.unpack_from(">HHB", data, f)
        render, priority, alpha, face_skins, face_tex, vskins, maya = data[f + 5:f + 12]
        tex_types = data[:texs]
        start = texs + verts
        if render == 1:
            start += faces
        start += faces                                          # face strip types
        if priority == 255:
            start += faces
        if face_skins == 1:
            start += faces
        del tex_types, alpha, face_tex
    elif n >= 23 and data[-2] == 0xFF and data[-1] == 0xFE:    # type 2
        f = n - 23
        verts, faces, texs = struct.unpack_from(">HHB", data, f)
        render, priority, alpha, face_skins, vskins, maya = data[f + 5:f + 11]
        start = verts + faces
        if priority == 255:
            start += faces
        if face_skins == 1:
            start += faces
        if render == 1:
            start += faces
    else:
        return None
    if maya != 1:
        return None
    b = _Buf(data, start + (verts if vskins == 1 else 0))
    out = []
    for _ in range(verts):
        out.append([(b.u8(), b.u8()) for _ in range(b.u8())])
    return out

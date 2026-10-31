# The geometry of the original (STARTL.EXE): from track piece to screen polygon

Reference for the geometry stage of the original renderer. Read from `STARTL.EXE` (disassembly
with `tools/dis.sh -f`) and checked against the running original (DOSBox, track 1 start grid,
near chase camera, `tools/dosram.py`). It describes what the program does, shortcuts included,
not a cleaned-up version, so that the geometry can be reproduced bit for bit in integer
arithmetic. What neither code nor RAM settles is marked **open**.

The fill routines themselves (spans, texels, blending, 2D clipping at the screen edge) are
described in [original_filler.md](original_filler.md); this document ends at their entry
(section 8).

## Verified against the original (RAM against the formulas in this document)

| Stage | Result |
|---|---|
| Camera matrices A and B from the angles (section 1) | bit-identical |
| Object matrices, block matrices, all four chains W, F, G1, G (section 4) | bit-identical, 4 blocks of a car |
| Camera-space position and sort key of all 149 drawn objects (section 3) | bit-identical |
| Object visibility test: 149 of 189 accepted (section 3) | same set |
| Cell window (section 2) | same columns |
| Vertex transform and projection, 103 vertices (section 5) | bit-identical |
| Face row and back-face test, 30 of 48 faces in the same order (section 6) | bit-identical |
| `TABS\PERSP01.TAB` against the formula (section 5) | all 65664 entries |

Only read, not checked in RAM: the clippers (section 7), the 2D accept/reject in the setup
(section 8), the reflection path (section 8.5), the cell window in every detail.

## 0. Conventions

- Addresses are "flat" as in `tools/dis.sh -f` and openscreamer `docs/NOTES.md` (code from
  0x10000, data from 0x100000). Pointers in the running program are linear guest addresses; in
  the DOSBox session used, flat 0x10000 is at linear 0x1d2000 and flat 0x100000 at linear
  0x2ae000. Many graphics variables live in the code object (0x82xxx, 0x84xxx, 0x8axxx).
- Self-modifying code: immediates `0xaaaabbbb` / `-0x55554445` are filled in at run time;
  "patched" means a value is written into the code that way.
- `imul` is 32 bit (overflow wraps), `sar n` is arithmetic, i.e. **rounds down**
  (`floor(x / 2^n)`), `idiv` rounds **toward zero**. In the formulas `>>` = sar, `/` = idiv.
- Matrices: 3x3 as 9 int32 `M[0..8]`, often followed by a 3-int32 translation `T[0..2]`
  (12 int32 = one "affine set"). A point is a **row vector**, index `3*i+j`: row `i` = input
  axis, column `j` = output axis:
  `out[j] = ((in[0]*M[j] + in[1]*M[3+j] + in[2]*M[6+j]) >> 14) + T[j]`
  (add the three products in 32 bit first, then shift once).
- World axes: x, height, z (record order: +8 x, +0xc height, +0x10 z).
- View space (camera space): origin = camera position; **x positive = left in the image**,
  y = up, **z negative = forward**. The projection eye is 128 units behind the camera (at
  z = +128), the projection plane at z = 0, scale 128.

## 1. The camera

### 1.1 From the float camera set to the integer view structure (0x1a134)

The camera set `C` (ds:0x10a1c, flat 0x110a1c) holds the position as **int32** at +0, +4, +8
(x, height, z) and the angles as **float** in radians at +0x30 (pitch), +0x34 (heading),
+0x38 (roll). The view structure `V` lies directly before it, **flat 0x11099c** (= C - 0x80);
`ds:0x8a4fc` points to it, `[ds:0x8a594]` = 0x8a4fc. 0x1a134 writes:

```c
/* x87: fld float32, fmul float32 1024.0 (0x1008f8), fdiv float32 pi (0x1008fc = 3.14159274),
   0x4d7b4: frndint with rounding TOWARD ZERO, fistp */
V.pitch   = (int32)trunc((C.pitch   [+ C.dpitch]) * 1024.0f / PI_F) & 0x7ff;   /* V+0 */
V.heading = (int32)trunc((C.heading [+ C.dhead ]) * 1024.0f / PI_F) & 0x7ff;   /* V+4 */
V.roll    = (int32)trunc((C.roll    [+ C.droll ]) * 1024.0f / PI_F) & 0x7ff;   /* V+8 */
V.x = C.x [+ C.dx];  V.h = C.h [+ C.dh];  V.z = C.z [+ C.dz];                  /* V+0xc.. */
```

The bracketed parts (floats C+0x3c..0x44, ints C+0x24..0x2c) only apply during the fly-over
between two views (`[C+0x58] != 0`). Truncate toward zero, then `& 0x7ff`: a negative angle is
therefore rounded up (-162.97 -> -162 -> 1886). In RAM: pitch 6.09469 -> 1986, heading 3.14424
-> 1024, position (2603, 543, -19776).

### 1.2 The sine table

0x30190 (in the code object, i.e. in the EXE): 2048 entries of 8 bytes, `int16 sin` at +0,
`int16 cos` at +4 (values x 32768, entry 0 = (0, 32767), entry 1 = (100, 32767)). Always used as

```c
void sincos(int a, int32 *s, int32 *c) { *s = tab[a].sin >> 1; *c = tab[a].cos >> 1; } /* 16384 = 1 */
```

Note: cos 0 = 32767 >> 1 = **16383**, not 16384. Every matrix built from angles is therefore
slightly smaller than a rotation (diagonal at angle 0: 16382).

### 1.3 Matrix A (local -> world, 0x81d90) and matrix B ("inverse", 0x81e34)

0x8b360 builds two affine sets from a set `X` (+0 pitch p, +4 heading h, +8 roll r, +0xc..+0x14
position): A at X+0x18 (T at X+0x3c) and B at X+0x48 (T at X+0x6c).

```c
void matA(int p, int h, int r, int32 M[9]) {          /* 0x81d90 */
    int32 s0,c0,s1,c1,s2,c2; sincos(p,&s0,&c0); sincos(h,&s1,&c1); sincos(r,&s2,&c2);
    int32 t  = (s1*s0) >> 14;
    int32 t2 = (c1*s0) >> 14;
    M[0] = (c2*c1 - t*s2) >> 14;
    M[1] = (s2*c0) >> 14;
    M[2] = (t2*s2 + s1*c2) >> 14;
    M[3] = (-(c2*t + c1*s2)) >> 14;     /* negate BEFORE shifting */
    M[4] = (c2*c0) >> 14;
    M[5] = (t2*c2 - s1*s2) >> 14;
    M[6] = (-(c0*s1)) >> 14;
    M[7] = -s0;                          /* unshifted */
    M[8] = (c1*c0) >> 14;
}

void matB(int p, int h, int r, int32 B[9]) {          /* 0x81e34 with the angles 0x7ff - a */
    int32 s0,c0,s1,c1,s2,c2;
    sincos(0x7ff - p,&s0,&c0); sincos(0x7ff - h,&s1,&c1); sincos(0x7ff - r,&s2,&c2);
    int32 t = (s0*s1) >> 14;
    int32 u = (s0*c1) >> 14;
    B[0] = (c1*c2 + t*s2) >> 14;
    B[1] = (c1*s2 - t*c2) >> 14;
    B[2] = (s1*c0) >> 14;
    B[3] = -((c0*s2) >> 14);             /* negate AFTER shifting */
    B[4] = (c0*c2) >> 14;
    B[5] = s0;                           /* unshifted */
    B[6] = (u*s2 - s1*c2) >> 14;
    B[7] = (-(u*c2) - s1*s2) >> 14;
    B[8] = (c0*c1) >> 14;
}

void trans(const int32 P[3], const int32 M[9], int32 T[3]) {   /* 0x81d40 */
    for (j = 0; j < 3; j++) T[j] = (P[0]*M[j] + P[1]*M[3+j] + P[2]*M[6+j]) >> 14;
}

void build(X) {                                          /* 0x8b360 */
    matA(X.p, X.h, X.r, X.A);  X.A.T = X.pos;
    matB(X.p, X.h, X.r, X.B);  trans(-X.pos, X.B, X.B.T);
}
```

**Shortcut 0x7ff - a:** B uses the angles `0x7ff - a = -(a+1) mod 2048`, not `0x800 - a` (another
routine, 0x8b440, uses 0x800). B is therefore not the inverse of A but (checked over 20000
random angles) that of A(a+1): `|B(a) - A(a+1)^T| <= 4`, `|B(a) - A(a)^T|` up to 109.
Consequence: **the image is drawn with camera angles that are each larger by 1/2048 turn** than
those of the camera; for objects this only affects the eye point of the face selection
(section 6). In RAM at heading 1024 / roll 0: B = (-16384, 41, 49, 50, 16096, 3048, -41, 3047,
-16097), i.e. non-zero off the diagonal.

### 1.4 View structure per frame (0x8a9c8, called from 0x1ffdd)

```c
build(V);                       /* A_cam at V+0x18, B_cam at V+0x48 */
sincos copies (0x829e3): 0x8228c.. = sin/cos(p,h,r), 0x822a4.. = sin/cos(0x7ff - p,h,r)
CAM = V.A.T  (= V.x, V.h, V.z) to 0x8aae4..0x8aaec;
V.A.T = V.B.T = 0;              /* both camera matrices without translation */
0x8aab0 = B_cam * 6.1035e-5f    /* only for the reflection path */
```

From here on everything is computed relative to the camera: positions are shifted by CAM in
integers, then rotated.

## 2. Candidates: which pieces are considered at all

### 2.1 Cell window (0x82e88)

Two fixed points in the camera ground plan, `(-57433, -56096)` and `(57433, -56096)`
(0x82b08/0x82b10, 0x82b14/0x82b1c; x, z each), are rotated by the heading, with
s = 0x822ac = sin(0x7ff - h), c = 0x822b0 = cos(0x7ff - h):

```c
X = (x*c + z*s) >> 14;   Z = (z*c - x*s) >> 14;      /* for both points */
x0 = min(X1, X2); x1 = max(X1, X2); z0 = min(Z1, Z2); z1 = max(Z1, Z2);
x0 = min(x0, -0x2000); z0 = min(z0, -0x2000); x1 = max(x1, 0x2000); z1 = max(z1, 0x2000);
x0 += V.x; x1 += V.x; z0 += V.z; z1 += V.z;  clamp to [-0x100000, 0xfffff];
WIN = (x0, z0, x1, z1)                                   /* 0x82b54.. , for moving objects */
```

Pitch and roll are not used. The window is then intersected with the view box of the cell the
camera is in (MMP cell, run 4: `[cell+0x24] > 0`, box at `[cell+0x10]`: int32 x0, -, z0, x1, -,
z1): per axis each box edge is clamped to the window; only if the two clamped values differ do
they replace the window (0x8303c..). The result (+0x100000) is at 0x82b9c. Rows and columns of
the MMP grid (8192 units):

```c
col0 = x0 >> 13;  ncol = (x1 - x0) >> 13;  if (col0 + ncol <= 0xfd) ncol++;
row0 = z0 >> 13;  nrow = (z1 - z0) >> 13;  if (row0 + nrow <= 0xfd) nrow++;
visited: rows row0 .. row0+nrow, columns col0 .. col0+ncol (both inclusive)
```

This is **not** `x1 >> 13 - x0 >> 13`; it can be one column more or fewer than the ones
geometrically touched. Checked: col0*4 = 496, ncol = 6 in RAM as computed.

### 2.2 The in-range list (0x83114 -> 0x83164)

Row by row (z ascending), within it column by column (x ascending), per cell **run 1 first,
then run 0** (records of 0x40 bytes, in stored order):

```c
rec[+4] = 0;
d2 = (int64)(x-V.x)^2 + (h-V.h)^2 + (z-V.z)^2;   /* 64 bit */
d2q = (uint32)(d2 >> 2);                          /* shrd: only the low 32 bits afterwards */
if (d2q < 676000000) list[n++] = { d2q, rec };    /* 0x500f0, distance < 52000 */
```

### 2.3 Moving objects (0x8a6a8)

From the field `[ds:0x8426c]` (pointer to records, 0 = end, bit 30 set = skip): the position
must lie in `WIN` (x0 <= x <= x1, z0 <= z <= z1), then the same distance test; appended as
`{ d2q, rec | 0x40000000 }` after the static ones. Total count at ds:0x82248.

## 3. Object level: view space, key, visibility (0x8a7b0)

For each list entry, in list order:

```c
d = rec.pos - CAM;                                     /* int32 */
v = (d * B_cam) >> 14   (shifted once per column)      /* x, y, z in view space */
key = -( round_ne(sqrt(d2q)) >> 2 );                   /* fild, fsqrt, fistp: round to even */
if (moving) key += 800;
switch (mesh[+0x32c]) { 1: key -= 1500; 2: key -= 800; 3: key -= 6000; 4: key -= 200; }
/* key is an int16 (word arithmetic) */

r = mesh[+0];                                          /* radius */
if (v.z - r > 0) reject;                               /* entirely behind the camera */
e = PERSP[v.z - r];                                    /* at the nearest point, section 5 */
if (v.z + r <= FAR) reject;                            /* FAR = ds:0xd7a20 */
if ((((int64)(v.x - r) * e) >> 17) + 80  < 0  ) reject;   /* half resolution */
if ((((int64)(v.x + r) * e) >> 17) + 80  > 159) reject;
if ((((int64)(v.y - r) * e) >> 17) + 65  < 0  ) reject;
if ((((int64)(v.y + r) * e) >> 17) + 65  > 100) reject;
draw[m++] = (key << 16) | (0x14 * m_old);              /* 0x55550; data at 0x52cb0 */
```

`sqrt(d2q)` is `round(d/2)`, `>> 2` makes it about d/8. Exact in integers:
`r = isqrt(n); if (n - r*r > r) r++;` (a tie cannot occur for integer n). The constants 80, 159,
65, 100 are ds:0x8a530, 0x8a518, 0x8a534, 0x8a51c (view setting from 0x4c610; different in the
cockpit). The `>> 17` are the low 32 bits of a 64-bit `shrd`.

Sorting (0x832c0): ascending by the whole int32 (key in the upper word, list position in the
lower) - **far first**, in list order for equal keys. The result is unique, so the algorithm (a
quicksort variant) does not matter.

## 4. Per object and per block: the matrices

### 4.1 Data structures

- Object record `rec` (0x40 bytes): +0 mesh index, +4 flags (bit 15 = billboard), +8 x,
  +0xc height, +0x10 z, +0x14 pitch, +0x18 heading, +0x1c roll (0..2047), +0x20 animation phase
  (negative = clock), +0x3c reflection frame. Moving objects (cars): from rec+0x40, 0x20 bytes
  per block: +8 pitch, +0xc heading, +0x10 roll, +0x14 x, +0x18 height, +0x1c z (relative to
  the object).
- Mesh = `[[ds:0x12153c] + 4*rec[0]]`: +0 radius, +0x10 number of frames (-1 = none), +0x14
  number of blocks, **+0x18 + 4b + 0x104*L** offset of block b at level of detail L (0, 1, 2;
  0 = no block), +0x32c class.
- Block: +0 radius, +4 centre (3 x int16), +0xc offset of the position set (from the mesh),
  +0x18 flags, +0x1c vertices, +0x20 normals (reflection), +0x24 texture coordinates (all from
  the mesh), +0x28 16 x int32 offsets of the face rows (from the block), +0x68 name.
- Position set of a static block (mesh + [blk+0xc]): +4 pitch, +8 heading, +0xc roll, +0x10 x,
  +0x14 height, +0x18 z; with frames from +0x1c, per frame 0x30 bytes = 12 int16 (A_blk: 9
  matrix + 3 translation) and 12 int16 (B_blk).
- Object state `O` = 0x841a4, block state `P[b]` = 0x8428c + 0x1f4*b, block order 0x8a434.

### 4.2 Object (0x8aafc, 0x8ac58)

```c
O.mesh = mesh; O.nblk = mesh[+0x14];
phase = rec[+0x20] >= 0 ? rec[+0x20] >> 2 : (clock & 0x3fff);      /* 0x84284; clock = ds:0x121538 */
refl  = rec[+0x3c];                                                  /* 0x84288 */
ang   = (rec.p, rec.h, rec.r);
if (rec[+4] & 0x8000)          /* billboard, 0x4bca0 */
    ang = ((rec.p - V.p) & 0x7ff,  (-(rec.h - V.h + 0x400)) & 0x7ff,  0x7ff - V.r);
X = { ang, rec.pos - CAM };  build(X);       /* A_obj = O+0x48 (T = relpos), B_obj = O+0x78 */
frame = mesh[+0x10] >= 0 ? ((mesh[+0x10] * clock) >> 16) * 0x30 : -1;   /* 0x84278 */
```

### 4.3 Block (static 0x8ae38, moving 0x8ad4c), then the chain

```c
blk0 = mesh + mesh[+0x18 + 4*b];                     /* level of detail 0 */
if (moving) Y = { sub.p, sub.h, sub.r, sub.pos };    /* rec+0x40+0x20*b */
else        Y = { pr.p, pr.h, pr.r, pr.pos };        /* position set */
back-face test normal (jl);                           /* 0x8ad02 */
if (frame >= 0) {                                     /* 0x8acf0, 0x8ad14 */
    back-face test inverted (jge);
    A_blk = sx(frame words 0..11);  B_blk = sx(frame words 12..23);   /* 16383 = 1 */
} else build(Y);                                      /* A_blk = P+0x28, B_blk = P+0x58 */

W  = mul(A_blk, A_obj);        /* P+0xa0  block -> world relative to camera */
F  = mul(W,     B_cam);        /* P+0x118 block -> view space, B_cam.T = 0 */
G1 = mul(A_cam, B_obj);        /* P+0xd0  view space -> object */
G  = mul(G1,    B_blk);        /* P+0x148 view space -> block; G.T = camera in the block */

void mul(L, R, D) {            /* 0x82018: L first, then R */
    for i, j: D[3i+j] = (L[3i]*R[j] + L[3i+1]*R[3+j] + L[3i+2]*R[6+j]) >> 14;
    for j:    D.T[j]  = ((L.T[0]*R[j] + L.T[1]*R[3+j] + L.T[2]*R[6+j]) >> 14) + R.T[j];
}
```

Each chain step rounds once per entry. A track piece with heading 0 has W[0] = 16380, not
16384: vertices appear shrunk by about 0.03 % (1 unit at 4000), which is part of the original.

### 4.4 Block key and level of detail

Static (0x8b04c):

```c
m = F applied to blk0+4 (centre);                      /* view space */
dist = round_ne(sqrt(m.x^2 + m.y^2 + m.z^2));          /* x87, exactly like isqrt with rounding */
key = -(dist >> 1);
if (name[0] == 'Z') key += 0x2000;
L = key > [0x8a504] ? 0 : key > [0x8a508] ? 1 : 2;     /* -6000, -8000 in RAM */
```

Moving (0x8af24):

```c
dist = round_ne(sqrt(...)); key0 = -dist;
key = key0;
if (name[0] != 'A' && name[0] != 'a' && name[3] == 0) {     /* exactly three characters */
    if (name[2] == 'i') key -= 4000;      /* tyres, inner wheel arches */
    if (name[2] == 'x') key -= 1000;      /* hubs */
}
L = key0 > [0x8a550] ? 0 : key0 > [0x8a554] ? 1 : 2;  /* -4000, -7000; without the name surcharge */
```

Then for both: `blk = mesh[+0x18 + 4*b + 0x104*L]` (0 -> block dropped), `P.blk = mesh + blk`,
order word `0x8a434[b] = (key << 16) | b`; after all blocks sorted ascending (0x832c0): **far
block first**. The matrices stay those of the block at level 0; the level of detail only swaps
vertices and faces.

**Levels of detail are used on track 1** (e.g. mesh 185 `a5trb`: blocks at 816, 1536, 2224; mesh
0 `ax1sa`: 816, 5928, 6876). Static blocks switch at a block-centre distance of about 12000 and
16000 (the `'Z'` surcharge shifts this), cars at 4000 and 7000.

## 5. Vertex transform and projection (0x8a628 -> 0x83380)

0x8a628 patches F into the code (0x83320), takes vertex, normal and UV pointers from the block
(0x841bc, 0x841c0, 0x841c4) and skips blocks without a level of detail. 0x83380 computes **all**
vertices of the block until an x = 0x7fff appears (end marker instead of a count):

```c
for (off = 0; (x = vert[off].x) != 0x7fff; off += 6) {
    vx = ((x*F[0] + y*F[3] + z*F[6]) >> 14) + F.T[0];
    vy = ((x*F[1] + y*F[4] + z*F[7]) >> 14) + F.T[1];
    vz = ((x*F[2] + y*F[5] + z*F[8]) >> 14) + F.T[2];
    VIEW[2*off] = { vx, vy, vz };                 /* 0x5f0c8, 12 bytes per vertex */
    if (vz > 0) SCR[off] = 0x10001000;            /* behind the camera: marker */
    else {
        e  = PERSP[vz];                           /* negative */
        sx = (int32)(vx*e + 0x9f0000) >> 16;      /* = floor(vx*e / 65536) + 159, 32-bit product */
        sy = ((int32)(vy*e) >> 16) + [0x8a52c];   /* + 130 (eye line) */
        SCR[off] = (sx << 16) | (sy & 0xffff);    /* 0x64e94, index = vertex byte offset */
    }
}
```

**PERSP01.TAB** (`TABS\`, 262656 bytes = 65664 int32, loaded unchanged, base patched at 0x83424
and 0x8a923): entry k belongs to z = k - 65536, i.e. z in [-65536, 127], and for all entries

```c
PERSP[z] = -(int32)(8388608 / (128 - z));        /* 2^23/(128-z), truncated toward zero */
```

z = 0 -> -65536, z = -128 -> -32768, z = -43808 -> -190. So `sx = 159 - 128*x/(128 - z)` with the
eye 128 behind the camera, image centre **column 159** (fixed in the code, not 160). The table
can be replaced by exactly this division. Overflow: `vx*e` is 32 bit; from `|vx| >= 256*(128 -
vz)` it wraps, but such corners are far outside (section 8.2 then usually rejects the polygon).

## 6. Which faces are drawn (0x83468)

### 6.1 Eye point and face row

```c
E = G.T + blk.mid;                                   /* camera in the block, plus block+4 */
E12 = E * 0x13333 >> 16  per axis;                   /* x 1.2 (78643/65536) */
if (one of the three products overflows 32 bit) E12 = E;   /* then all three unscaled */
row = (E12.y < 0) ? 8 : 0;                           /* 0x8a598 */
if (E12.x >= 0) row += (E12.z >= 0) ? (E12.z >  E12.x ? 0 : 1) : (-E12.z <= E12.x ? 2 : 3);
else            row += (E12.z <  0) ? (-E12.z > -E12.x ? 4 : 5) : ( E12.z <= -E12.x ? 6 : 7);
face row = blk + blk[+0x28 + 4*row];                 /* int16 face offsets, <= 0 = end */
```

The eye point is the **camera** (not the projection eye 128 behind it), in block coordinates
through G (i.e. with the 0x7ff angle error of the B matrices), plus the block centre, times 1.2.
This is a shortcut of the original, not the exact test; reproduce it as is.

Beforehand, only with block flag 0x2000: 0xc1fc0 (reflection coordinates, section 8.5).

### 6.2 Back-face test

Per face of the row, in row order (`face = blk + off`):

```c
v = vert[face[+8]];                                  /* first corner, int16 */
d = (v.x - E12.x)*N.x + (v.y - E12.y)*N.y + (v.z - E12.z)*N.z;   /* N = face+0x14.., int32, 32 bit */
visible = normal ? (d >= 0) : (d < 0);              /* jl or jge (objects with frames) */
if (visible) list += { (int32)(int16)face[+4], face };          /* 0x5d188 */
```

There is no screen-space test (no cross product), no frustum test per block, and none per face
other than the classes below. The filler (0xd55ff), however, silently checks the winding: one
edge chain walks the corners forward, the other backward, and a span only arises where the
first lies to the right of the second (`sub`/`jg` at 0xd5969 and 0xd59eb). Nothing appears of a
face whose corners run the other way on screen, i.e. one facing away from the projection eye
(128 behind the camera), even if it passes the E12 test. Checked in RAM (car selection,
SHADOW): the face list 0x5d188 of the last drawn block (rauto) contains exactly the 30 faces
that the E12 test lets through, 11 of them facing away from the projection eye; likewise the
front nose face of fauto passes the test but does not cover the front tyre in the image.

### 6.3 Block classes: simple, near, far

```c
zb = F.T[2];  rb = blk[+0];                          /* block origin in view space, radius */
if (zb + rb >= -8)        class NEAR;
else if (zb - rb <= FAR)  class FAR;                 /* FAR = ds:0xd7a20 */
else                      class SIMPLE;              /* only 6.2 */
```

**FAR** (0x83908): after the back-face test, the four corners (`face+8, +0xa, +0xc, +0xe`; for
triangles +0xe is the repeated first corner) against FAR (patched, 0xffff54e0 = -43808 in the
file): all `z > FAR` -> stays; all `z <= FAR` -> dropped; mixed -> far clipper
`[sel + 0x822ec]` immediately (section 7).

**NEAR** (0x8368a): after the back-face test, first a coarse rejection with `TABS\VIS.TAB`
(40256 int32, entry `VIS[z + 40128]` for z in [-40128, 127], at flat 0xb3218 for z = 0; values
about 0.633*(128 - z), no simple formula, so take it from the file), K = `face[+0x20]` (equal
per block, about twice the block radius):

```c
bool ok(c, axis) { z = VIEW[c].z - K;  if (z >= 0) return false;
                   return VIS[z] + K >= abs(VIEW[c].axis); }
if (!(ok(c0,x) || ok(c1,x) || ok(c2,x) || ok(c0,y) || ok(c1,y) || ok(c2,y) || ok(c3,x)))
    face dropped;
```

(exactly these seven checks in this order; c3 only with x.) Then z against 0: corner 0 `z >= 0`
and all others `z >= 0` -> dropped; all `z < 0` -> stays; mixed -> near clipper
`[sel + 0x822e8]` immediately. Whether the VIS rejection ever hits a visible face is **open**
(unlikely at K = 2r, but not excluded).

A block in NEAR is not additionally cut at FAR.

## 7. Clipping

Both clippers are Sutherland-Hodgman against one plane over the closed corner loop (n edges),
work on VIEW (view space) and write a polygon into the buffer `[ds:0x792ac]`:

```c
struct ClipPoly { int32 matword; int32 n; struct { int32 scr; int16 u, v; } c[n]; };
```

`scr` as SCR (sx high, sy low). The entry `{matword, face}` in the face list is replaced by
`{matword + 4, &ClipPoly}` (the +4 later selects the entry "clipped polygon"); with fewer than 3
corners (byte compare `al < 3`) or the overflow flag set, it is deleted. Output order: P0 if
inside; then per edge P->Q: inside/inside -> Q; inside/outside -> S; outside/inside -> S, Q;
outside/outside -> nothing; finally the duplicate P0 is dropped.

### 7.1 Near (0xd7308, texture family; plane z = 0, inside = z <= 0)

```c
/* I = inner corner, O = outer corner, the interpolation base is always I */
num = 0 - I.z   /* patched 'add ecx,0' at 0xd7470: 0 */;  den = O.z - I.z;
x' = (int32)(((int64)(O.x - I.x) * num) / den) + I.x;     /* idiv: toward zero */
y' = (int32)(((int64)(O.y - I.y) * num) / den) + I.y;
sy = (int16)((y' << 7) / [0x829ac]) + 130;   /* /(-128): = 130 - y' */
sx = (int16)((x' << 7) / [0x829ac]) + 159;   /* = 159 - x' */
/* 16-bit addition overflows -> value 0x7fff and polygon rejected (0xd7a1c = -1) */
U(c) = (raw_u(c) * mat[+8]) >> 5;            /* as in the setup */
u' = (int16)((((int64)(((raw_u(O) - raw_u(I)) * mat[+8]) >> 5)) * num) / den) + U(I);
v' likewise with raw_v and mat[+0xc];
```

The intersections lie at z = 0, so their projection is exactly `159 - x'`, `130 - y'` (as
PERSP[0] = -65536). Inner corners keep their SCR value and their scaled u, v.

### 7.2 Far (0xd76a0; inside = z >= FAR)

As 7.1 with `num = FAR - I.z` (<= 0, via ds:0xd7a24 = -FAR), `den = O.z - I.z` (< 0) and

```c
sy = (int16)((y' << 7) / [0x829b0]) + 130;   /* 0x829b0 = FAR - 128, in RAM -43936 */
sx = (int16)((x' << 7) / [0x829b0]) + 159;
/* overflow -> 0x7fff, but NO rejection */
```

Note: rounded toward zero here, rounded down in the table path.

FAR comes from the detail setting through 0x8baa4 (writes 0xd7a20, 0xd7a24 = -FAR,
0x829b0 = FAR - 128 and the immediates of class FAR); table at 0x21d4: -27808, -27808, -43808
(in RAM: -43808).

Other families (table 8.1): flat colour 0xd80a0 / 0xd8280 (buffer with colour word, corners only
`scr`, 4 bytes), reflection 0xda500 / 0xda844 (u, v from the reflection buffer, scaled **with**
the material size), 192/208 0xca6e0 / 0xd8280. Not read in detail (**open**).

## 8. Hand-over to the fill routines

### 8.1 List, dispatcher, routine table

After the faces of a block, 0x840fc draws the list 0x5d188 in its order:

```c
for (each {w, p} until w == -1) {
    mat = MLB + (w & ~0xf);                 /* MLB = [ds:0x121540] */
    call [ mat[+0] + (w & 0xf) + 0x822e0 ](p);
}
```

`mat[+0]` (routine number, a multiple of 16) selects a row of four entries: +0 face, +4 clipped
polygon, +8 near clipper, +0xc far clipper. In RAM:

| Number | +0 face | +4 clipped | +8 near | +0xc far | Meaning (NOTES) |
|---|---|---|---|---|---|
| 0, 16, 32 | d5350 | d51e0 | d7308 | d76a0 | texture |
| 48 ... 112 | dc1b4 | dc078 | d7308 | d76a0 | texture, texel 0 transparent (64) |
| 128, 144 | d8844 | d8744 | d80a0 | d8280 | single colour (colour = face+2) |
| 160, 176 | db310 | db1c0 | da500 | da844 | reflection |
| 192, 208 | cae84 | cad84 | ca6e0 | d8280 | colour from face+2, kind **open** |
| 224, 240 | cd324 | cd1e8 | d7308 | d76a0 | transparent through MAPn.SMG |

On track 1, according to NOTES: 16, 64, 128, 160 (one material), 224.

### 8.2 Texture setup (0xd5350 face, 0xd51e0 clipped)

```c
mat = MLB + face[+4]  -> ds:0x84270;   n = face[+6] -> ds:0x8b564;
for (i = 0; i < n; i++) {                       /* corners face+8, +0xa, +0xc, (+0xe) */
    o = face[+8 + 2*i];
    poly[i].scr = SCR[o];                       /* int16 sy, int16 sx */
    poly[i].u = (int16)((uv[o].u * mat[+8])  >> 5);   /* 1/8 texel: raw*width/256*8, rounded down */
    poly[i].v = (int16)((uv[o].v * mat[+0xc]) >> 5);
}
poly[n..2n-1] = poly[0..n-1];                    /* buffer 0xd7a60, twice in a row */
```

224 (cd324) uses `raw << 3` for width and height 256 (same result), otherwise the same. Then
accept/reject (both entries equal except for one detail, see below) with min/max over the
corners:

```c
reject if: any of min_sy, max_sy, min_sx, max_sx <= -16000 or > 16000;
           min_sx == max_sx (no width);
           max_sy < 0;  max_sx < 0;  min_sx > 319;  min_sy > H;   /* H = view height, 200 */
fully inside if: min_sy >= 0 && min_sx >= 0 && max_sy <= H && max_sx <= 319
    -> filler without edge clipping: 0xd55ff (both entries, 0xd51e0 via 0xd5327);
otherwise -> 2D clipper at the screen edge: 0xd4b60 (face) or 0xd52e0 -> 0xd4b79 (clipped).
```

The detail: in 0xd5350 min_sx and max_sx are compared as a doubleword with max_sy in the low
word; a polygon whose leftmost corner lies exactly on column 319 with max_sy > 0 is therefore
dropped, and one with max_sx = 319 goes to the clipper. 0xd51e0 compares without the low word.
H is patched into the code (0xd65e0 from ds:0x8a514).

From here the filler takes over (see [original_filler.md](original_filler.md)).

### 8.3 Texture address and frame (in the filler, 0xd5980)

```c
tex = MLB + mat[+4];   frames = tex[+0];
frame = (frames * phase) >> 14;                    /* phase = ds:0x84284, section 4.2 */
pixel = MLB + tex[+8 + 4*frame];                   /* rows of 256 bytes */
/* reflection (0xdb8c8): frame = (frames * refl) >> 16, refl = rec[+0x3c] */
```

### 8.4 Flat colour (128, 0xd8844)

Colour = `(int16)face[+2]` -> ds:0xda4d4; corners only `scr` (4 bytes, buffer 0x5c9b8, doubled),
otherwise the same min/max check.

### 8.5 Reflection (160): u, v per vertex in floating point (0xc1fc0, 0xc2230)

Only blocks with flag 0x2000 (on all tracks the 49 blocks with vertex normals, mainly cars).
Before the faces, per vertex i (normals at `[blk+0x20]`: int32 count, then float x, y, z):

```c
n  = (n_obj * F[0..8]) * 6.1035e-5f;               /* into view space, float */
p  = VIEW[i] (int -> float);  l = sqrt(p.p);
e  = p / l;                                        /* stored as float32 */
dd = 2 * (e.n);   r = dd*n - e;
u  = fistp(127 * r.x) + 127 - offx;                /* fistp: round to even */
v  = fistp(127 * r.z) + 127 - offz;                /* r.z, not r.y */
REFL[off] = (v << 16) | (u & 0xffff);              /* 0xc3bd4, index = vertex byte offset */
```

`offx`, `offz` = `& 0xff` of two float accumulators in the object: only for a moving object
(pointer `[0x7427c]` with bit 30), 0xc1fc0 takes the path since the last draw (position -
rec+0x24, whole units, initially 0) as float, rotates it with 0x8aab0, multiplies by the factor
at **0xc2428** (a constant in the code), and adds it to rec+0x30, +0x34, +0x38; `fist` (round
to nearest) `& 0xff` of the first and third sums are the offsets; otherwise 0. The factor is
0.008 in the program; **0x1813c** (driving physics of the player car, only in state +0x148 = 0,
i.e. not in a crash) sets it every frame according to the kind of the last marker reached (car
+0x66c = `MAPn.MRK` +4, written by 0x1820c): kind 0 -> 0.002 (`0x3b03126f`), kinds 1 and 2 ->
0.03 (`0x3cf5c28f`), others leave it. It applies to all objects.

**The reflection frame:** 0x1813c sets car +0x7b0 = 0 / 0x8000 / 0xff00 for kind 0 / 1 / 2 in
the same step; 0x1607c copies it every frame into the object set (rec+0x3c), 0x8aafc to
[0x84288], and the filler takes frame `(frames * [0x84288]) >> 16` of the texture record
(0xdb8c8). On track 1 the texture of the 160 material has three frames (254 x 254): 0 the sky, 1
(kind 1, markers 3 and 4: the tunnel) the lights of the tunnel ceiling, 2 a third. Opponents do
not call 0x1813c (their +0x7b0 stays 0 from 0x14095): only the player car reflects the tunnel.

Setup 0xdb310 takes `u = REFL.u << 3` (without the material size), the near clipper 0xda500
`(REFL.u * mat[+8]) >> 5` - a difference within the original. The texel address is taken
`& 0xffff` (0xdb59b): u, v wrap beyond 256. The x87 intermediates run in 80 bit with individual
float32 stores. That v comes from r.z is as written in the code (NOTES says "sideways, up").

Checked in RAM (DOSBox, track 1 start grid, near chase camera): object set of the player (mesh
229) with rec+0x24 = (2600, 0, -18500), accumulators (-5.086755, -0.209960, 37.013203), hence
offx = 251, offz = 37; rec+0x3c = 0; factor at 0xc2428 = 0.002 (driving physics already runs on
the start grid). The table 0xc3bd4 (6 bytes per vertex, index = vertex byte offset) then holds
the 128 values of the player's body.

## 9. Order within a frame (0x81870)

1. 0x83a68 background.
2. 0x83114: cell window, list of pieces in range (2.1, 2.2); 0x8a6a8 appends the moving objects
   (2.3).
3. 0x8a7b0: view-space position, key, visibility (3); 0x832c0 sorts: far first.
4. Per object in this order: 0x8ac58 computes matrices, key and level of detail of all blocks
   and sorts the blocks (far first); then per block in this order: 0x8a628 (patch in F, skip
   block without level), 0x83380 (all vertices), 0x83468 (faces in row order, clippers
   immediately in place), 0x840fc (fill polygons, list order).

Pure painter's algorithm without a depth buffer: objects far -> near, blocks of an object far
-> near, faces of a block in the precomputed row order.

## 10. Open points

- Whether the VIS.TAB rejection (6.3) ever hits visible faces, and a formula for VIS.TAB.
- Routines 192/208 (cae84): what they do and whether they occur on any track.
- The clippers of the families flat, reflection, 192 in detail (section 7, last paragraph).
- Reflection path (8.5): where the accumulators of a car get their value on the start grid (the
  first draw adds the whole position, rotated with the camera of that moment, during the camera
  swing) cannot be recomputed without the history. The constants 0xc242c (9.99) and 0xc2430
  (1024, a dead compare) are unused.
- P+0x1f0 (rounded distance of the block origin, summed in float32, 0x83487) is written, no
  reader found; likewise ds:0x8b554 = 0x17700000 per block.
- The view settings (0x8a510..0x8a53c) for views other than the 200-row one (cockpit: 132) are
  not read out.
- Where the cell box (run 4 of an MMP cell) comes from and whether every cell has one.

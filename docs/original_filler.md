# The polygon fillers of the original (STARTL.EXE)

Reference for how the original renderer fills polygons. Read from the code (`tools/dis.sh -f`;
flat addresses as in openscreamer `docs/NOTES.md`: code from 0x10000, data from 0x100000) and,
where the code modifies itself or tables are built at run time, from the memory of the running
original (`tools/dosram.py`, track 1, race at the start grid, paused). This file starts at the
entry of the filler; how corners are projected to whole pixels is described in
[original_geometry.md](original_geometry.md). Unverified points are listed under **Open points**.

## Summary

- **Columns, not rows.** Every filler walks the polygon edges **in x** (one edge step per pixel
  column, integer x, no subpixels) and then draws **vertical spans**, column by column from the
  right (maxX) to the left (minX), top to bottom within each column.
- **Affine, no perspective.** u, v are interpolated linearly in x along the upper and lower edge
  and linearly in y within the column. No per-pixel division, no subdivision, no depth buffer, no
  lighting, no dithering.
- **Two reciprocal tables replace every division:** `T1[dx]` (about 65536/dx) for the edges,
  `T2[n]` = floor(32767/n) for the columns. Both round down; `T1` additionally has an error at
  every multiple of 11.
- **Integer corners:** corner = (x, y) integers, packed as `(x << 16) | (y & 0xffff)`; u, v per
  corner in eighths of a texel (3 fraction bits).
- A column covers rows `floor(yo) ... floor(yu)` **inclusive** (routines 16, 64, 160; the last
  row repeats the texel of the one before), routine 224 covers rows `floor(yo) ... floor(yu)-1`.
  Columns with `floor(yu) - floor(yo) <= 0` stay empty (only the flat-colour routine 128 draws
  1 pixel there).
- Texture: 256 bytes per row, `texel = data[(v & 255) * 256 + (u & 255)]`; u and v wrap modulo
  256 (no clamping to the material size).

## 1. Dispatch and the routines used on track 1

### Routine table `0x822e0`

A material (16 bytes in `MAPn.MLB`, base `[0x121540]`) starts with its routine number `R`. The
call target is `[0x822e0 + R + k]`, `k` = variant:

| `k` | when | Input |
|---|---|---|
| 0 | face entirely in front of the near plane and of the far plane | pointer to the face record |
| 4 | polygon already built as a corner list by a clipper (k = 8 or 12) | pointer to the corner list |
| 8 | face crosses the near plane (z = 0) | face record (ebp), clips in 3D |
| 12 | face crosses the far plane | face record (ebp), clips in 3D |

Routines used on track 1 (from the material table in RAM: 251 x 16, 91 x 64, 9 x 128, 2 x 224,
1 x 160):

| R | what | k=0 | k=4 | k=8 | k=12 |
|---|---|---|---|---|---|
| 16 | opaque texture | `0xd5350` | `0xd51e0` | `0xd7308` | `0xd76a0` |
| 64 | texture, texel 0 transparent | `0xdc1b4` | `0xdc078` | `0xd7308` | `0xd76a0` |
| 224 | blend through `MAPn.SMG` | `0xcd324` | `0xcd1e8` | `0xd7308` | `0xd76a0` |
| 128 | single colour, no texture | `0xd8844` | `0xd8744` | `0xd80a0` | `0xd8280` |
| 160 | mirror (u, v from `0xc3bd4`) | `0xdb310` | `0xdb1c0` | `0xda500` | `0xda844` |

(The table has further entries, e.g. R = 48 = same as 64 and R = 192 = `0xcae84`, that do not
occur on track 1.)

### Draw list

The face loops (`0x83680` with near test, `0x83940` with far test) write one pair
`(material offset | k, pointer)` per visible face into a list and then classify the four corners
(for triangles corner 3 = corner 0) by camera depth `fs:[corner*2]`:

```
near test (0x83878): all z >= 0  -> remove the pair (entirely behind the camera)
                     mixed       -> call [0x822e8 + R] (k = 8) immediately; the clipper
                                    replaces the pair by (material+4, corner list)
                     all z < 0   -> pair stays (k = 0)
far test  (0x839af): all z <= zf -> remove; mixed -> k = 12; otherwise it stays.
                     zf = 0xffff54e0 = -43808 (DETAIL HI, patched into the code)
```

The list ends with `-1`. It is executed in **list order** by `0x84100` (painter's algorithm,
no z):

```
loop: m = *list++; if (m == -1) return;  p = *list++;
      cld; call [0x822e0 + mlb[m & ~15].routine + (m & 15)] (eax = p)
```

### Texture frame and frame buffer

```
mat   = MLB + material offset                (face record +0x04, int16)
rec   = MLB + mat[+4]                        (texture record)
frame = (rec[0] * [0x84284]) >> 14           (R = 16, 64, 224)
frame = (rec[0] * [0x84288]) >> 16           (R = 160)
tex   = MLB + rec[8 + 4*frame]               (patched into 4 load instructions of the column loop)
pixel(x, y) = [0x121524] + rowtab[y] + x,    rowtab = ds:0x20bb8 (= 320*y, entry 200 = 64000)
```

Confirmed in RAM: the texture frame patched into `0xd5a53` for the last drawn polygon is exactly
the one computed this way (material 213, 8 frames, frame 3).

## 2. Filler input

### Corner

Projected corner `P` = dword at `0x64e94 + corner offset` (corner offset = word from the face
record `+0x08...`, byte offset of the corner):

```
P = (x << 16) | (y & 0xffff)      x, y whole pixels (signed 16 bit)
```

### u, v per corner (routines 16, 64, 224)

```
raw_u, raw_v = int16 at [0x841c4] + corner offset, +2        (block +0x24, 0..255)
U8 = (int16)((raw_u * mat.extU) >> 5)                          (sar, 32 bit, keep the low 16 bit)
V8 = (int16)((raw_v * mat.extV) >> 5)
```

`U8`, `V8` are texels x 8. For `extU = extV = 256` routine 224 takes a shorter path (`raw << 3`)
that gives the same 16 bits. The corner list at `0xd7a60` has 8 bytes per corner,
`[P][U8 | V8 << 16]`, and is appended once for the edge walk (corner n...2n-1 = corner 0...n-1).
Only 3 or 4 corners (face record `+0x06`).

**Routine 160:** `raw` from the table `0xc3bd4` (computed per frame by `0xc2230`, see NOTES.md),
`U8 = (raw_u << 3) & 0xffff`, `V8` likewise, **without** the material size (the only 160
material is 254 x 254). The near clipper `0xda500`, however, uses `raw * ext >> 5`.

### Colour (routine 128)

`colour = (uint8)face record[+0x02]` (k = 0) or the third dword of the corner list (k = 4);
corners are `P` only (4 bytes per corner, list `0x5c9b8`).

### Corner list from the 3D clippers (k = 4)

`[material offset][n][P0][UV0]...[Pn-1][UVn-1]` (for 128: `[material][n][colour][P0]...`), in a
buffer at `[0x792ac]`.

## 3. 3D clipping at the near and far plane (k = 8, 12; `0xd7308`, `0xd76a0`)

Sutherland-Hodgman over the corners in face order. Camera coordinates: `z = fs:[o*2]`,
`x = gs:[o*2]`, `y = gs:[o*2+4]` (o = corner offset). Inside is `z <= 0` for the near plane and
`z >= zf` for the far plane (`[0xd7a20]` = -43808). Corners that are inside are passed on
unchanged with their `P` from `0x64e94`. Intersection (in both directions computed from the
**inner** corner `i`; the outer one is `a`; 32x32->64-bit product, `idiv` truncates toward zero):

```
num = -(z_i + E)          E = 0 (near), 43808 = [0xd7a24] (far)
den = z_a - z_i
yc  = (y_a - y_i) * num / den + y_i
xc  = (x_a - x_i) * num / den + x_i
Py  = (int16)( (yc << 7) / D + [0x8a52c] )     D = [0x829ac] = -128 (near), [0x829b0] = -43936 (far)
Px  = (int16)( (xc << 7) / D + 159 )           [0x8a52c] = 130
U8c = (((int16)(raw_u_a - raw_u_i) * extU) >> 5) * num / den + ((raw_u_i * extU) >> 5)
V8c = likewise with v / extV
```

If the 16-bit addition overflows (sign), the value becomes 0x7fff; the **near** clipper then
discards the polygon (`[0xd7a1c] = -1`), the far clipper does not. Fewer than 3 corners:
discarded. Note: the near test of the face loop counts `z = 0` as "behind", the clipper as
"inside". (The projection `x*128/(z-128)+159` matches D = -128 at z = 0 and -43936 at
z = -43808.)

## 4. Pre-check and 2D clipping

### Corner bounds

Over all corners (routine 16, k = 0; the others are equivalent):

```
imin = index of the smallest P (signed 32 bit, i.e. smallest x, then smallest y);
       on a tie the LATER corner wins
minY, maxY (int16), minX, maxX
```

Discarded (nothing drawn) if:
- a bound lies outside (-16000, +16000] (`0xc180`/`0x3e80`; for x over the whole dword),
- `minX == maxX` (zero columns wide),
- `maxY < 0` or `maxX < 0`,
- `minY > B` (B = `[0x8a514]` = 200, patched into the comparisons at start: `0xd65e0`,
  `0xdc8d0`, `0xdb9b0`, ...),
- k = 0: `((minX << 16) | maxY) > (319 << 16)`, i.e. `minX >= 320`, **or** `minX == 319` and
  `maxY > 0`; k = 4: only `minX > 319`.

If everything lies in `0 <= x <= 319`, `0 <= y <= B` (precisely: `minY >= 0`, `minX >= 0`,
`maxY <= B`, `(maxX << 16 | maxY) <= 319 << 16`), it goes straight to the edge walk, otherwise
through the 2D clipper first.

### 2D clipper (`0xd4b60`; 64: `0xdbbe0`, 224: `0xccd50`, 160: `0xdaba4`, same code;
128: `0xd8444` without u, v)

Sutherland-Hodgman in four stages, each only if needed:

| Stage | when | inside |
|---|---|---|
| x = 0 | `minX < 0` | `x >= 0` |
| y = 0 | `minY < 0` | `y >= 0` |
| x = 319 | `maxX >= 319` | `x <= 319` (constant, not `[0x8a510]`) |
| y = B | `maxY >= B` | `y <= B` (B = `[0x8a514]` = 200) |

After each stage: fewer than 3 corners means nothing is drawn. The intersection is always
computed from the **outer** corner `a` (so both directions give the same point), all in
**16 bit**: `imul r16` (16x16->32), `idiv r16` (toward zero), result the low 16 bits. Example for
the plane x = c (c = 0 or 319), inner corner `i`:

```
y   = y_a + (int16)( (x_a - c) * (y_a - y_i) / (x_i - x_a) )   with (x_a-c)*(y_a-y_i) as a
                                                                  16x16 product
u8  = U8_a + (int16)( (U8_i - U8_a) * (c - x_a) / (x_i - x_a) )
v8  = V8_a + (int16)( (V8_i - V8_a) * (c - x_a) / (x_i - x_a) )
P   = (c << 16) | (y & 0xffff)
```

The plane y = c is analogous with the roles swapped (`x = x_a + (x_i-x_a)*(c-y_a)/(y_i-y_a)`,
u, v with `(c - y_a)/(y_i - y_a)`). The differences are 16-bit differences, the products fit in
32 bit, the quotient must fit in 16 bit (otherwise #DE; unreachable for |coordinate| < 16000).
Afterwards `imin` is determined again from the clipped list (same rule) and the same edge walk
is used.

## 5. Edge walk (routine 16: `0xd55ff`...; 64: `0xdc443`; 224: `0xcd5bf`; 160: `0xdb59b`;
128: `0xd8958`)

Two chains start at corner `imin`: **A forward** (corner imin, imin+1, ...) and **B backward**
(imin, imin-1, ...). Edges with `dx <= 0` (16-bit difference) are skipped in both chains. A is
the **upper** and B the **lower** boundary; this holds only if the corners run **clockwise** on
screen (y down). Counter-clockwise, `yu - yo <= 0` everywhere and the polygon stays invisible
(a second, implicit back-face criterion).

Per edge (start s, end e, `dx = x_e - x_s > 0`):

```
stepY = (int16)(y_e - y_s) * T1[dx]                  16.16, exact 32-bit product
Y     = y_s << 16
routine 16:        stepU = p >> 11, +1 if p < 0           p = (int16)(U8_e - U8_s) * T1[dx]
routines 64, 224:  stepU = p >> 11                         (sar, round down)
                   U = (int16)U8_s << 5                     -> U, V are 8.8 (texel * 256)
routine 160:       stepU = p >> 4,  U = (int16)U8_s << 12   -> 17.15 (texel * 32768)
V likewise.        Routine 128 has Y only.
```

The walk goes column by column from `x = minX` to `maxX`; for each column an entry
`(YB, YA, UB, VB, UA, VA)` is stored (table on the stack from `0xba470` downward, 24 bytes per
column; 128: `(YB, YA)` from `0xba498`, 8 bytes, unrolled loop via jump table `0xd89f0`). Per
column: write the entry, then add each quantity's step (32 bit, overflow as in hardware).

End of an edge: when a chain reaches its end corner (same x), the **column of that corner is
rewritten**: the chain whose edge ends starts the next edge with the **exact corner values**
(`Y = y << 16`, `U = U8 << 5`), the other chain keeps its **accumulated** value. If both end at
the same column, both restart (`0xd5623`). In the **last column (maxX)** the accumulated values
of both chains are stored, not corner values. The walk ends when both chains arrive at the same
corner (pointer comparison `0xd5623`).

```
c   = x_s;  A = edge(A); B = edge(B)
loop:
  xe = min(A.xend, B.xend)
  for (; ; c++) { column[c] = (B.Y, A.Y, B.U, B.V, A.U, A.V);
                  if (c == xe) break;  A += A.step;  B += B.step; }
  if (done) break
  if (A.xend == xe) A = next edge(A) with start values from the corner   // overwrites column[xe]
  if (B.xend == xe) B = next edge(B) with start values from the corner   // on the next pass
```

Consequence of the rounding (to be reproduced bit-exactly): `T1` rounds down, so a **downward**
edge (dy > 0) ends just below `y_e << 16`; if that is the accumulated chain (e.g. in column
maxX), it ends one row too early. With dx = 1, `T1[1] = 65535`, so always.

### Table T1 (`0x5b26c`, fixed in the EXE, 704 entries, identical in RAM)

```
T1[0] = T1[1] = 65535
T1[n] = 65536 / (n + 1)   if n % 11 == 0      (table error: 11 -> 5461 instead of 5957)
T1[n] = 65536 / n         otherwise            (rounded down), 2 <= n <= 703
```

Verified over all 702 entries. After clipping, dx is at most 319.

## 6. Drawing columns

Columns are drawn from `x = maxX` down to `minX`. `yo = YA >> 16`, `yu = YB >> 16` (arithmetic,
so rounded down), `n = yu - yo`.

- At the right end, columns are skipped while `yo < 0` or `yu < 0` or `n <= 0` (`0xd5954`);
  after that only for `n <= 0` (`0xd5a00`). The end is at column minX or at the guard above the
  table (`(0, 0x08000000)`, i.e. `yo = 0x800`).
- Routines 16, 64, 160: **n + 1 pixels**, rows `yo ... yu`; row `yu` gets the same texel as row
  `yu - 1` (`0xd5aca`). Routine 224: **n pixels**, rows `yo ... yu-1`.
- Routine 128: the first drawn column needs `n > 0`, after that every column with `n >= 0` is
  drawn, **n + 1 pixels** in the colour (1 pixel for n = 0).
- Row B = 200 can be written (clipping at y <= 200 inclusive, the row table has entry 200). It
  lies behind the visible 320 x 200 image.

### Texture step in the column, routines 16, 64, 224 (`0xd5a0e`...)

One divisor per column through `T2`, u with 16 fraction bits, v with 8, packed into two
registers:

```
U0 = UA, V0 = VA                  routines 64, 224
U0 = UA & ~15, V0 = VA & ~15      routine 16 (1/16 texel grid)
pu = (int32)((UB - U0) * T2[n])           // 32 bit, overflow as in hardware
r  = rol32(pu, 9)
cu = r & 0xff                              // integer u step (mod 256) = pu[30:23]
pv = (int32)((VB - V0) * T2[n]) >> 15      // 8.8, rounded down
S  = (r & 0xffff0000) | (pv & 0xffff)      // [u fraction 16 bit = pu[22:7]][v step 8.8]
E  = ((U0 & 0xff) << 24) | (V0 & 0xffff)   // [u fraction 8 bit][0][v 8.8]
ui = (U0 >> 8) & 0xff
for (i = 0; i < n; i++) {
    t = tex[((E >> 8) & 0xff) * 256 + ui];          // v = E[15:8], u = ui, both mod 256
    write(x, yo + i, t);
    s = (uint64)E + S;  E = (uint32)s;
    ui = (ui + cu + (s >> 32)) & 0xff;              // carry from the u fraction
}
if (R != 224) write(x, yo + n, t);                  // last texel repeats
```

Peculiarities: the u step never ends exactly at UB (`T2[n]` = floor(32767/n) < 32768/n); the
carry of v (bit 15 to bit 16) lands in the lowest bit of the u fraction (with a negative v step
almost every pixel, order of 2^-16 texel). The calculation uses the state **before** the step.

### Routine 160 (`0xdb928`...)

```
du = (UB - UA) >> 7;  dv = (VB - VA) >> 7;  ua = UA >> 7;  va = VA >> 7   // 8.8
us = du / n;  vs = dv / n                    // idiv, toward zero
cu = (us >> 8) & 0xff
S  = ((us & 0xff) << 24) | (vs & 0xffff)     // bits 23..16 = 0
E  = ((ua & 0xff) << 24) | (va & 0xffff)     // bits 23..16 = 0
ui = (ua >> 8) & 0xff
loop as above (n pixels, then the last texel once more).
```

Here u has only 8 fraction bits; the v carries collect in bits 23..16 and never reach u within
one column (<= 201 pixels).

### Table T2 (`0xd5af0`, generated at run time)

`T2[n] = 32767 / n` (rounded down) for 1 <= n <= 699, `T2[0] = 0x80000000` (never used);
verified in RAM for all entries.

## 7. What makes up a pixel

| R | Pixel |
|---|---|
| 16 | `t` |
| 64 | `t`, but write nothing if `t == 0` (also for the repeated last pixel) |
| 224 | `SMG[t * 256 + screen]` (segment `fs = [0x12149c]`; `t` is not limited, the table has 64 rows) |
| 128 | `colour` |
| 160 | `t` |

Nothing else: no shading, no dithering, no z inside the polygon (columns and rows of one
polygon do not overlap), no relation to the sky (the background is already in the frame buffer
and is painted over). Shared edges of neighbouring polygons are drawn by both (column x_e
belongs to both, row floor(yu) of the upper is floor(yo) of the lower); the later one in the
list wins.

## 8. Differences between the routines

| | 16 | 64 | 224 | 160 | 128 |
|---|---|---|---|---|---|
| Edge u/v | `>>11`, +1 if negative | `>>11` | `>>11` | `>>4` (17.15) | - |
| Column start | `& ~15` | - | - | `>>7` | - |
| Column u step | `T2`, 16-bit fraction | as 16 | as 16 | `idiv`, 8-bit fraction | - |
| Pixels per column | n+1 | n+1 | n | n+1 | n+1 (also n = 0) |
| Frame | `>>14`, `[0x84284]` | as 16 | as 16 | `>>16`, `[0x84288]` | - |

## Open points

- **Checked against the frame buffer only partly:** the last triangle of the image (routine 16,
  corners (97,154), (124,153), (126,158)) recomputed with the model above: all 96 pixels are in
  the right place with the same texel pattern, but the frame buffer (`0xdc30`) holds other
  colour numbers, consistently by a fixed mapping (70->67, 69->66, 71->67, 72->68, 74->83,
  90->52). The source of this recolouring (pause image? a later pass?) is open; a matching
  `SMG` row was not found in RAM.
- Who writes `T2` (`0xd5af0`) and the guard pairs `(0, 0x08000000)` above `0xba488` is not
  found (values taken from RAM).
- What lies in row 200 of the frame buffer and whether it is ever visible.
- Non-convex polygons and corners with equal P: the walk assumes convex polygons; the original's
  behaviour otherwise is not investigated.
- The face pre-selection against the side planes (`0x836d7`..., table `0xb3218`) and which of
  the two face loops an object gets: see the geometry doc.
- `0xdb9d0` (another clipper, reads from the stack) is not reached through the routine table;
  its purpose is open.

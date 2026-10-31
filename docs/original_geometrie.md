# Die Geometrie des Originals (STARTL.EXE): vom Streckenstück zum Bildschirm-Polygon

Stand 2026-10-29. Gelesen aus `STARTL.EXE` (Disassembly mit `tools/dis.sh -f`) und am laufenden
Original geprüft (DOSBox, Startaufstellung Strecke 1, nahe Verfolgerkamera, `tools/dosram.py`).
Ziel: eine Vorlage, nach der sich die Geometrie Bit für Bit in Integer-C und danach in kleiner
Hardware nachbauen lässt. Beschrieben ist, was das Programm tut, mit seinen Abkürzungen, nicht
eine bereinigte Fassung. Was weder Code noch RAM festlegen, ist mit **offen** markiert.

Die Füllroutinen selbst (Spans, Texel, Mischen, 2D-Clipping am Bildrand) stehen nicht hier; dieses
Dokument endet an ihrem Einsprung (Abschnitt 8).

## Am Original geprüft (RAM gegen Formeln aus diesem Dokument, Skripte im Scratchpad)

| Stufe | Ergebnis |
|---|---|
| Kameramatrix A und B aus den Winkeln (Abschnitt 1) | bitgleich |
| Objektmatrizen, Blockmatrizen, alle vier Verkettungen W, F, G1, G (Abschnitt 4) | bitgleich, 4 Blöcke eines Autos |
| Kamera-Raum-Position und Sortierschlüssel aller 149 gezeichneten Objekte (Abschnitt 3) | bitgleich |
| Sichtbarkeitstest der Objekte: 149 von 189 angenommen (Abschnitt 3) | dieselbe Menge |
| Zellenfenster (Abschnitt 2) | gleiche Spalten |
| Vertex-Transformation und Projektion, 103 Vertices (Abschnitt 5) | bitgleich |
| Flächenzeile und Rückseitentest, 30 von 48 Flächen in derselben Reihenfolge (Abschnitt 6) | bitgleich |
| `TABS\PERSP01.TAB` gegen die Formel (Abschnitt 5) | alle 65664 Einträge |

Nicht am RAM geprüft, nur gelesen: die Clipper (Abschnitt 7), die 2D-Annahme/Verwerfung im Setup
(Abschnitt 8), der Reflexionspfad (Abschnitt 8.5), das Zellenfenster in allen Einzelheiten.

## 0. Konventionen

- Adressen sind „flach“ wie in `tools/dis.sh -f` und openscreamer `docs/NOTES.md` (Code ab
  0x10000, Daten ab 0x100000). Zeiger im laufenden Programm sind lineare Adressen des Gasts; in
  dieser DOSBox-Sitzung liegt flach 0x10000 bei linear 0x1d2000 und flach 0x100000 bei linear
  0x2ae000. Viele Variablen der Grafik liegen im Codeobjekt (0x82xxx, 0x84xxx, 0x8axxx).
- Selbstmodifizierender Code: Immediates `0xaaaabbbb` / `-0x55554445` werden zur Laufzeit
  eingesetzt; „gepatcht“ heißt hier, dass ein Wert so in den Code geschrieben wird.
- `imul` ist 32 Bit (Überlauf wickelt), `sar n` ist arithmetisch, also **abrunden**
  (`floor(x / 2^n)`), `idiv` rundet **zur Null**. In den Formeln: `>>` = sar, `/` = idiv.
- Matrizen: 3×3 als 9 int32 `M[0..8]`, oft mit 3 int32 Verschiebung `T[0..2]` dahinter
  (12 int32 = ein „Affin-Satz“). Ein Punkt ist ein **Zeilenvektor**, Index `3*i+j`: Zeile `i` =
  Eingangsachse, Spalte `j` = Ausgangsachse:
  `out[j] = ((in[0]*M[j] + in[1]*M[3+j] + in[2]*M[6+j]) >> 14) + T[j]`
  (erst die drei Produkte in 32 Bit addieren, dann einmal schieben).
- Achsen der Welt: x, Höhe, z (die Reihenfolge der Datensätze: +8 x, +0xc Höhe, +0x10 z).
- Sicht-Raum (Kamera-Raum): Ursprung = Kameraposition; **x positiv = links im Bild**, y = oben,
  **z negativ = nach vorn**. Das Auge der Projektion liegt 128 Einheiten hinter der Kamera
  (bei z = +128), die Projektionsebene bei z = 0, Maßstab 128.

## 1. Die Kamera

### 1.1 Vom Float-Kamerasatz zur Int-Sichtstruktur (0x1a134)

Der Kamerasatz `C` (ds:0x10a1c, flach 0x110a1c) hat die Position als **int32** bei +0, +4, +8
(x, Höhe, z) und die Winkel als **float** im Bogenmaß bei +0x30 (Neigung), +0x34 (Kurs),
+0x38 (Rollen). Die Sichtstruktur `V` liegt unmittelbar davor, **flach 0x11099c** (= C − 0x80);
`ds:0x8a4fc` zeigt auf sie, `[ds:0x8a594]` = 0x8a4fc. 0x1a134 schreibt:

```c
/* x87: fld float32, fmul float32 1024.0 (0x1008f8), fdiv float32 pi (0x1008fc = 3.14159274),
   0x4d7b4: frndint mit Rundung ZUR NULL, fistp */
V.pitch   = (int32)trunc((C.pitch   [+ C.dpitch]) * 1024.0f / PI_F) & 0x7ff;   /* V+0 */
V.heading = (int32)trunc((C.heading [+ C.dhead ]) * 1024.0f / PI_F) & 0x7ff;   /* V+4 */
V.roll    = (int32)trunc((C.roll    [+ C.droll ]) * 1024.0f / PI_F) & 0x7ff;   /* V+8 */
V.x = C.x [+ C.dx];  V.h = C.h [+ C.dh];  V.z = C.z [+ C.dz];                  /* V+0xc.. */
```

Die Klammerteile (Floats C+0x3c..0x44, Ints C+0x24..0x2c) kommen nur beim Überflug zwischen
zwei Ansichten dazu (`[C+0x58] != 0`). Abschneiden zur Null, dann `& 0x7ff`: ein negativer Winkel
wird also aufgerundet (−162,97 → −162 → 1886). Im RAM: Neigung 6.09469 → 1986, Kurs 3.14424 →
1024, Position (2603, 543, −19776).

### 1.2 Die Sinustabelle

0x30190 (im Codeobjekt, also in der EXE): 2048 Einträge zu 8 Byte, `int16 sin` bei +0, `int16 cos`
bei +4 (Werte × 32768, Eintrag 0 = (0, 32767), Eintrag 1 = (100, 32767)). Benutzt wird immer

```c
void sincos(int a, int32 *s, int32 *c) { *s = tab[a].sin >> 1; *c = tab[a].cos >> 1; } /* 16384 = 1 */
```

Achtung: cos 0 = 32767 >> 1 = **16383**, nicht 16384. Jede aus Winkeln gebaute Matrix ist
daher etwas kleiner als eine Drehung (Diagonale bei Winkel 0: 16382).

### 1.3 Matrix A (lokal → Welt, 0x81d90) und Matrix B („invers“, 0x81e34)

0x8b360 baut aus einem Satz `X` (+0 Neigung p, +4 Kurs h, +8 Rollen r, +0xc..+0x14 Position)
zwei Affin-Sätze: A bei X+0x18 (T bei X+0x3c) und B bei X+0x48 (T bei X+0x6c).

```c
void matA(int p, int h, int r, int32 M[9]) {          /* 0x81d90 */
    int32 s0,c0,s1,c1,s2,c2; sincos(p,&s0,&c0); sincos(h,&s1,&c1); sincos(r,&s2,&c2);
    int32 t  = (s1*s0) >> 14;
    int32 t2 = (c1*s0) >> 14;
    M[0] = (c2*c1 - t*s2) >> 14;
    M[1] = (s2*c0) >> 14;
    M[2] = (t2*s2 + s1*c2) >> 14;
    M[3] = (-(c2*t + c1*s2)) >> 14;     /* negieren VOR dem Schieben */
    M[4] = (c2*c0) >> 14;
    M[5] = (t2*c2 - s1*s2) >> 14;
    M[6] = (-(c0*s1)) >> 14;
    M[7] = -s0;                          /* ungeschoben */
    M[8] = (c1*c0) >> 14;
}

void matB(int p, int h, int r, int32 B[9]) {          /* 0x81e34 mit den Winkeln 0x7ff - a */
    int32 s0,c0,s1,c1,s2,c2;
    sincos(0x7ff - p,&s0,&c0); sincos(0x7ff - h,&s1,&c1); sincos(0x7ff - r,&s2,&c2);
    int32 t = (s0*s1) >> 14;
    int32 u = (s0*c1) >> 14;
    B[0] = (c1*c2 + t*s2) >> 14;
    B[1] = (c1*s2 - t*c2) >> 14;
    B[2] = (s1*c0) >> 14;
    B[3] = -((c0*s2) >> 14);             /* negieren NACH dem Schieben */
    B[4] = (c0*c2) >> 14;
    B[5] = s0;                           /* ungeschoben */
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

**Abkürzung 0x7ff − a:** B nimmt die Winkel `0x7ff − a = −(a+1) mod 2048`, nicht `0x800 − a`
(eine andere Routine, 0x8b440, nimmt 0x800). B ist deshalb nicht das Inverse von A, sondern
(geprüft über 20000 Zufallswinkel) das von A(a+1): `|B(a) − A(a+1)ᵀ| ≤ 4`, `|B(a) − A(a)ᵀ|` bis 109.
Folge: **das Bild wird mit Kamerawinkeln gezeichnet, die je um 1/2048 Umdrehung größer sind**
als die der Kamera; bei den Objekten betrifft es nur den Augpunkt der Flächenwahl (Abschnitt 6).
Im RAM bei Kurs 1024 / Rollen 0: B = (−16384, 41, 49, 50, 16096, 3048, −41, 3047, −16097),
also nicht null außerhalb der Diagonale.

### 1.4 Sichtstruktur pro Bild (0x8a9c8, Aufruf aus 0x1ffdd)

```c
build(V);                       /* A_cam bei V+0x18, B_cam bei V+0x48 */
sincos-Kopien (0x829e3): 0x8228c.. = sin/cos(p,h,r), 0x822a4.. = sin/cos(0x7ff - p,h,r)
CAM = V.A.T  (= V.x, V.h, V.z) nach 0x8aae4..0x8aaec;
V.A.T = V.B.T = 0;              /* beide Kameramatrizen ohne Verschiebung */
0x8aab0 = B_cam * 6.1035e-5f    /* nur für den Reflexionspfad */
```

Ab hier rechnet alles relativ zur Kamera: Positionen werden ganzzahlig um CAM verschoben, dann
gedreht.

## 2. Kandidaten: welche Stücke überhaupt betrachtet werden

### 2.1 Zellenfenster (0x82e88)

Zwei feste Punkte im Kamera-Grundriss, `(−57433, −56096)` und `(57433, −56096)` (0x82b08/0x82b10,
0x82b14/0x82b1c; je x, z), werden mit dem Kurs gedreht, mit s = 0x822ac = sin(0x7ff − h),
c = 0x822b0 = cos(0x7ff − h):

```c
X = (x*c + z*s) >> 14;   Z = (z*c - x*s) >> 14;      /* für beide Punkte */
x0 = min(X1, X2); x1 = max(X1, X2); z0 = min(Z1, Z2); z1 = max(Z1, Z2);
x0 = min(x0, -0x2000); z0 = min(z0, -0x2000); x1 = max(x1, 0x2000); z1 = max(z1, 0x2000);
x0 += V.x; x1 += V.x; z0 += V.z; z1 += V.z;  auf [-0x100000, 0xfffff] begrenzen;
WIN = (x0, z0, x1, z1)                                   /* 0x82b54.. , für bewegte Objekte */
```

Neigung und Rollen gehen nicht ein. Dann wird das Fenster mit dem Sichtkasten der Zelle
geschnitten, in der die Kamera steht (MMP-Zelle, Lauf 4: `[cell+0x24] > 0`, Kasten bei
`[cell+0x10]`: int32 x0, –, z0, x1, –, z1): pro Achse wird jede Kastenkante auf das Fenster
geklemmt; nur wenn die beiden geklemmten Werte verschieden sind, ersetzen sie das Fenster
(0x8303c..). Ergebnis (+0x100000) bei 0x82b9c. Zeilen und Spalten des MMP-Gitters (8192 Einheiten):

```c
col0 = x0 >> 13;  ncol = (x1 - x0) >> 13;  if (col0 + ncol <= 0xfd) ncol++;
row0 = z0 >> 13;  nrow = (z1 - z0) >> 13;  if (row0 + nrow <= 0xfd) nrow++;
besucht: Zeilen row0 .. row0+nrow, Spalten col0 .. col0+ncol (beide einschließlich)
```

Das ist **nicht** `x1 >> 13 − x0 >> 13`; es kann eine Spalte mehr oder weniger sein als die
geometrisch berührten. Geprüft: col0·4 = 496, ncol = 6 im RAM wie gerechnet.

### 2.2 Die Liste in Reichweite (0x83114 → 0x83164)

Zeilenweise (z aufsteigend), darin spaltenweise (x aufsteigend), pro Zelle **erst Lauf 1, dann
Lauf 0** (Datensätze zu 0x40 Byte, Reihenfolge wie gespeichert):

```c
rec[+4] = 0;
d2 = (int64)(x-V.x)^2 + (h-V.h)^2 + (z-V.z)^2;   /* 64 Bit */
d2q = (uint32)(d2 >> 2);                          /* shrd: nur die unteren 32 Bit danach */
if (d2q < 676000000) liste[n++] = { d2q, rec };   /* 0x500f0, Abstand < 52000 */
```

### 2.3 Bewegte Objekte (0x8a6a8)

Aus dem Feld `[ds:0x8426c]` (Zeiger auf Datensätze, 0 = Ende, Bit 30 gesetzt = überspringen):
Position muss in `WIN` liegen (x0 ≤ x ≤ x1, z0 ≤ z ≤ z1), dann derselbe Abstandstest; angehängt
als `{ d2q, rec | 0x40000000 }` hinter die statischen. Anzahl gesamt ds:0x82248.

## 3. Objektebene: Kamera-Raum, Schlüssel, Sichtbarkeit (0x8a7b0)

Für jeden Eintrag der Liste, in Listenreihenfolge:

```c
d = rec.pos - CAM;                                     /* int32 */
v = (d * B_cam) >> 14   (je Spalte einmal geschoben)   /* x, y, z im Sicht-Raum */
key = -( round_ne(sqrt(d2q)) >> 2 );                   /* fild, fsqrt, fistp: auf Gerade runden */
if (bewegt) key += 800;
switch (mesh[+0x32c]) { 1: key -= 1500; 2: key -= 800; 3: key -= 6000; 4: key -= 200; }
/* key ist ein int16 (Wortarithmetik) */

r = mesh[+0];                                          /* Radius */
if (v.z - r > 0) verwerfen;                            /* ganz hinter der Kamera */
e = PERSP[v.z - r];                                    /* am nächsten Punkt, Abschnitt 5 */
if (v.z + r <= FAR) verwerfen;                         /* FAR = ds:0xd7a20 */
if ((((int64)(v.x - r) * e) >> 17) + 80  < 0  ) verwerfen;   /* halbe Auflösung */
if ((((int64)(v.x + r) * e) >> 17) + 80  > 159) verwerfen;
if ((((int64)(v.y - r) * e) >> 17) + 65  < 0  ) verwerfen;
if ((((int64)(v.y + r) * e) >> 17) + 65  > 100) verwerfen;
draw[m++] = (key << 16) | (0x14 * m_alt);              /* 0x55550; Daten bei 0x52cb0 */
```

`sqrt(d2q)` ist `round(d/2)`, `>> 2` macht daraus ≈ d/8. Ganzzahlig exakt: `r = isqrt(n); if
(n - r*r > r) r++;` (ein Gleichstand kann bei ganzem n nicht vorkommen). Die Konstanten 80, 159,
65, 100 sind ds:0x8a530, 0x8a518, 0x8a534, 0x8a51c (Sichteinstellung von 0x4c610; im Cockpit
anders). Die `>> 17` sind die unteren 32 Bit eines 64-Bit-`shrd`.

Sortierung (0x832c0): aufsteigend nach dem ganzen int32 (Schlüssel im oberen Wort, Position in der
Liste im unteren) — **fern zuerst**, bei gleichem Schlüssel in Listenreihenfolge. Das Ergebnis
ist eindeutig, der Algorithmus (eine Quicksort-Spielart) also egal.

## 4. Pro Objekt und pro Block: die Matrizen

### 4.1 Datenstrukturen

- Objektdatensatz `rec` (0x40 Byte): +0 Mesh-Index, +4 Flags (Bit 15 = Billboard), +8 x,
  +0xc Höhe, +0x10 z, +0x14 Neigung, +0x18 Kurs, +0x1c Rollen (0..2047), +0x20 Animationsphase
  (negativ = Uhr), +0x3c Reflexionsbild. Bewegte Objekte (Autos): ab rec+0x40 je Block 0x20 Byte:
  +8 Neigung, +0xc Kurs, +0x10 Rollen, +0x14 x, +0x18 Höhe, +0x1c z (relativ zum Objekt).
- Mesh = `[[ds:0x12153c] + 4*rec[0]]`: +0 Radius, +0x10 Anzahl Frames (−1 = keine), +0x14 Anzahl
  Blöcke, **+0x18 + 4b + 0x104·L** Offset des Blocks b in Detailstufe L (0, 1, 2; 0 = kein
  Block), +0x32c Klasse.
- Block: +0 Radius, +4 Mitte (3 × int16), +0xc Offset des Positionssatzes (vom Mesh), +0x18 Flags,
  +0x1c Vertices, +0x20 Normalen (Reflexion), +0x24 Texturkoordinaten (alle vom Mesh),
  +0x28 16 × int32 Offsets der Flächenzeilen (vom Block), +0x68 Name.
- Positionssatz eines statischen Blocks (Mesh + [blk+0xc]): +4 Neigung, +8 Kurs, +0xc Rollen,
  +0x10 x, +0x14 Höhe, +0x18 z; mit Frames ab +0x1c je Frame 0x30 Byte = 12 int16 (A_blk: 9 Matrix
  + 3 Verschiebung) und 12 int16 (B_blk).
- Objektzustand `O` = 0x841a4, Blockzustand `P[b]` = 0x8428c + 0x1f4·b, Blockreihenfolge 0x8a434.

### 4.2 Objekt (0x8aafc, 0x8ac58)

```c
O.mesh = mesh; O.nblk = mesh[+0x14];
phase = rec[+0x20] >= 0 ? rec[+0x20] >> 2 : (clock & 0x3fff);      /* 0x84284; clock = ds:0x121538 */
refl  = rec[+0x3c];                                                  /* 0x84288 */
ang   = (rec.p, rec.h, rec.r);
if (rec[+4] & 0x8000)          /* Billboard, 0x4bca0 */
    ang = ((rec.p - V.p) & 0x7ff,  (-(rec.h - V.h + 0x400)) & 0x7ff,  0x7ff - V.r);
X = { ang, rec.pos - CAM };  build(X);       /* A_obj = O+0x48 (T = relpos), B_obj = O+0x78 */
frame = mesh[+0x10] >= 0 ? ((mesh[+0x10] * clock) >> 16) * 0x30 : -1;   /* 0x84278 */
```

### 4.3 Block (statisch 0x8ae38, bewegt 0x8ad4c), dann Verkettung

```c
blk0 = mesh + mesh[+0x18 + 4*b];                     /* Detailstufe 0 */
if (bewegt) Y = { sub.p, sub.h, sub.r, sub.pos };    /* rec+0x40+0x20*b */
else        Y = { pr.p, pr.h, pr.r, pr.pos };        /* Positionssatz */
Rückseitentest normal (jl);                           /* 0x8ad02 */
if (frame >= 0) {                                     /* 0x8acf0, 0x8ad14 */
    Rückseitentest umgedreht (jge);
    A_blk = sx(frame-Wörter 0..11);  B_blk = sx(frame-Wörter 12..23);   /* 16383 = 1 */
} else build(Y);                                      /* A_blk = P+0x28, B_blk = P+0x58 */

W  = mul(A_blk, A_obj);        /* P+0xa0  Block -> Welt relativ zur Kamera */
F  = mul(W,     B_cam);        /* P+0x118 Block -> Sicht-Raum, B_cam.T = 0 */
G1 = mul(A_cam, B_obj);        /* P+0xd0  Sicht-Raum -> Objekt */
G  = mul(G1,    B_blk);        /* P+0x148 Sicht-Raum -> Block; G.T = Kamera im Block */

void mul(L, R, D) {            /* 0x82018: erst L, dann R */
    for i, j: D[3i+j] = (L[3i]*R[j] + L[3i+1]*R[3+j] + L[3i+2]*R[6+j]) >> 14;
    for j:    D.T[j]  = ((L.T[0]*R[j] + L.T[1]*R[3+j] + L.T[2]*R[6+j]) >> 14) + R.T[j];
}
```

Jede Verkettung rundet einmal pro Eintrag. Ein Streckenstück mit Kurs 0 hat W[0] = 16380, nicht
16384: die Vertices erscheinen um ≈ 0,03 % verkleinert (bei 4000 Einheiten 1 Einheit), und das
gehört zum Original.

### 4.4 Blockschlüssel und Detailstufe

Statisch (0x8b04c):

```c
m = F angewandt auf blk0+4 (Mitte);                    /* Sicht-Raum */
dist = round_ne(sqrt(m.x^2 + m.y^2 + m.z^2));          /* x87, exakt wie isqrt mit Rundung */
key = -(dist >> 1);
if (name[0] == 'Z') key += 0x2000;
L = key > [0x8a504] ? 0 : key > [0x8a508] ? 1 : 2;     /* -6000, -8000 im RAM */
```

Bewegt (0x8af24):

```c
dist = round_ne(sqrt(...)); key0 = -dist;
key = key0;
if (name[0] != 'A' && name[0] != 'a' && name[3] == 0) {     /* genau drei Zeichen */
    if (name[2] == 'i') key -= 4000;      /* Reifen, Radkästen innen */
    if (name[2] == 'x') key -= 1000;      /* Naben */
}
L = key0 > [0x8a550] ? 0 : key0 > [0x8a554] ? 1 : 2;  /* -4000, -7000; ohne Namenszuschlag */
```

Dann für beide: `blk = mesh[+0x18 + 4*b + 0x104*L]` (0 → Block entfällt), `P.blk = mesh + blk`,
Reihenfolgewort `0x8a434[b] = (key << 16) | b`; nach allen Blöcken aufsteigend sortiert
(0x832c0): **ferner Block zuerst**. Die Matrizen bleiben die des Blocks in Stufe 0; die
Detailstufe tauscht nur Vertices und Flächen.

**Die Detailstufen werden auf Strecke 1 benutzt** (z. B. Mesh 185 `a5trb`: Blöcke bei 816, 1536,
2224; Mesh 0 `ax1sa`: 816, 5928, 6876). Statische Blöcke wechseln bei Abstand der Blockmitte
≈ 12000 und ≈ 16000 (der `'Z'`-Zuschlag verschiebt das), Autos bei 4000 und 7000.

## 5. Vertex-Transformation und Projektion (0x8a628 → 0x83380)

0x8a628 setzt F in den Code (0x83320), Vertex-, Normalen- und UV-Zeiger aus dem Block (0x841bc,
0x841c0, 0x841c4) und überspringt Blöcke ohne Detailstufe. 0x83380 rechnet **alle** Vertices des
Blocks, bis ein x = 0x7fff kommt (Endmarke statt Anzahl):

```c
for (off = 0; (x = vert[off].x) != 0x7fff; off += 6) {
    vx = ((x*F[0] + y*F[3] + z*F[6]) >> 14) + F.T[0];
    vy = ((x*F[1] + y*F[4] + z*F[7]) >> 14) + F.T[1];
    vz = ((x*F[2] + y*F[5] + z*F[8]) >> 14) + F.T[2];
    VIEW[2*off] = { vx, vy, vz };                 /* 0x5f0c8, 12 Byte je Vertex */
    if (vz > 0) SCR[off] = 0x10001000;            /* hinter der Kamera: Marke */
    else {
        e  = PERSP[vz];                           /* negativ */
        sx = (int32)(vx*e + 0x9f0000) >> 16;      /* = floor(vx*e / 65536) + 159, 32-Bit-Produkt */
        sy = ((int32)(vy*e) >> 16) + [0x8a52c];   /* + 130 (Augenlinie) */
        SCR[off] = (sx << 16) | (sy & 0xffff);    /* 0x64e94, Index = Vertex-Byteoffset */
    }
}
```

**PERSP01.TAB** (`TABS\`, 262656 Byte = 65664 int32, unverändert geladen, Basis bei 0x83424 und
0x8a923 gepatcht): Eintrag k gehört zu z = k − 65536, also z ∈ [−65536, 127], und es gilt für alle
Einträge

```c
PERSP[z] = -(int32)(8388608 / (128 - z));        /* 2^23/(128-z), zur Null abgeschnitten */
```

z = 0 → −65536, z = −128 → −32768, z = −43808 → −190. Damit ist `sx = 159 − 128·x/(128 − z)`
mit dem Auge 128 hinter der Kamera, Bildmitte **Spalte 159** (fest im Code, nicht 160).
Die Tabelle lässt sich durch genau diese Division ersetzen. Überlauf: `vx*e` ist 32 Bit; ab
`|vx| ≥ 256·(128 − vz)` wickelt es, solche Ecken liegen aber weit außerhalb (Abschnitt 8.2
verwirft dann meist das Polygon).

## 6. Welche Flächen gezeichnet werden (0x83468)

### 6.1 Augpunkt und Flächenzeile

```c
E = G.T + blk.mid;                                   /* Kamera im Block, plus Block+4 */
E12 = E * 0x13333 >> 16  je Achse;                   /* x 1.2 (78643/65536) */
if (eines der drei Produkte läuft in 32 Bit über) E12 = E;   /* dann alle drei unskaliert */
row = (E12.y < 0) ? 8 : 0;                           /* 0x8a598 */
if (E12.x >= 0) row += (E12.z >= 0) ? (E12.z >  E12.x ? 0 : 1) : (-E12.z <= E12.x ? 2 : 3);
else            row += (E12.z <  0) ? (-E12.z > -E12.x ? 4 : 5) : ( E12.z <= -E12.x ? 6 : 7);
zeile = blk + blk[+0x28 + 4*row];                    /* int16-Flächenoffsets, <= 0 = Ende */
```

Der Augpunkt ist die **Kamera** (nicht das Projektionsauge 128 dahinter), in Blockkoordinaten über
G (also mit dem 0x7ff-Winkelfehler der B-Matrizen), plus die Blockmitte, mal 1,2. Das ist eine
Abkürzung des Originals, nicht der saubere Test; nachbauen wie hier.

Vorab, nur bei Block-Flag 0x2000: 0xc1fc0 (Reflexionskoordinaten, Abschnitt 8.5).

### 6.2 Rückseitentest

Pro Fläche der Zeile, in Zeilenreihenfolge (`face = blk + off`):

```c
v = vert[face[+8]];                                  /* erste Ecke, int16 */
d = (v.x - E12.x)*N.x + (v.y - E12.y)*N.y + (v.z - E12.z)*N.z;   /* N = face+0x14.., int32, 32 Bit */
sichtbar = normal ? (d >= 0) : (d < 0);              /* jl bzw. jge (Objekte mit Frames) */
if (sichtbar) liste += { (int32)(int16)face[+4], face };          /* 0x5d188 */
```

Es gibt keinen Test im Bildraum (kein Kreuzprodukt), keinen Frustumtest pro Block und keinen
pro Fläche außer den folgenden Klassen. Der Füller (0xd55ff) prüft aber stillschweigend die
Drehrichtung: eine Kante läuft die Ecken vorwärts ab, die andere rückwärts, und eine Spanne
entsteht nur, wo die erste rechts der zweiten liegt (`sub`/`jg` bei 0xd5969 und 0xd59eb). Von
einer Fläche, deren Ecken auf dem Schirm andersherum laufen, also einer vom Projektionsauge
(128 hinter der Kamera) abgewandten, erscheint nichts, auch wenn sie den Test mit E12 besteht.
Im RAM geprüft (2026-10-31, Autoauswahl, SHADOW): die Flächenliste 0x5d188 des zuletzt
gezeichneten Blocks (rauto) enthält genau die 30 Flächen, die der Test mit E12 durchlässt,
darunter 11, die vom Projektionsauge abgewandt sind; ebenso besteht die vordere Nasenfläche von
fauto den Test, im Bild deckt sie den Vorderreifen trotzdem nicht.

### 6.3 Blockklassen: einfach, nah, fern

```c
zb = F.T[2];  rb = blk[+0];                          /* Blockursprung im Sicht-Raum, Radius */
if (zb + rb >= -8)        Klasse NAH;
else if (zb - rb <= FAR)  Klasse FERN;               /* FAR = ds:0xd7a20 */
else                      Klasse EINFACH;            /* nur 6.2 */
```

**FERN** (0x83908): nach dem Rückseitentest die vier Ecken (`face+8, +0xa, +0xc, +0xe`; bei
Dreiecken ist +0xe die wiederholte erste Ecke) gegen FAR (gepatcht, 0xffff54e0 = −43808 in der
Datei): alle `z > FAR` → bleibt; alle `z ≤ FAR` → fällt weg; gemischt → Fern-Clipper
`[sel + 0x822ec]` sofort (Abschnitt 7).

**NAH** (0x8368a): nach dem Rückseitentest zuerst eine grobe Verwerfung mit `TABS\VIS.TAB`
(40256 int32, Eintrag `VIS[z + 40128]` für z ∈ [−40128, 127], bei flach 0xb3218 für z = 0; Werte
≈ 0,633·(128 − z), keine einfache Formel, also aus der Datei nehmen), K = `face[+0x20]` (pro
Block gleich, ≈ doppelter Blockradius):

```c
bool ok(c, achse) { z = VIEW[c].z - K;  if (z >= 0) return false;
                    return VIS[z] + K >= abs(VIEW[c].achse); }
if (!(ok(c0,x) || ok(c1,x) || ok(c2,x) || ok(c0,y) || ok(c1,y) || ok(c2,y) || ok(c3,x)))
    Fläche fällt weg;
```

(genau diese sieben Prüfungen in dieser Reihenfolge; c3 nur mit x.) Danach z gegen 0: Ecke 0
`z ≥ 0` und alle anderen `z ≥ 0` → fällt weg; alle `z < 0` → bleibt; gemischt → Nah-Clipper
`[sel + 0x822e8]` sofort. Ob die VIS-Verwerfung je eine sichtbare Fläche trifft: **offen**
(bei K = 2r unwahrscheinlich, aber nicht ausgeschlossen).

Ein Block in NAH wird nicht zusätzlich an FAR geschnitten.

## 7. Clipping

Beide Clipper sind Sutherland–Hodgman an einer Ebene über die geschlossene Eckenschleife
(n Kanten), arbeiten auf VIEW (Sicht-Raum) und schreiben ein Polygon in den Puffer
`[ds:0x792ac]`:

```c
struct ClipPoly { int32 matword; int32 n; struct { int32 scr; int16 u, v; } c[n]; };
```

`scr` wie SCR (sx hoch, sy tief). Der Eintrag `{matword, face}` in der Flächenliste wird durch
`{matword + 4, &ClipPoly}` ersetzt (die +4 wählt später den Einsprung „geclipptes Polygon“);
bei weniger als 3 Ecken (Bytevergleich `al < 3`) oder gesetzter Überlaufmarke wird er gestrichen.
Reihenfolge der Ausgabe: P0 wenn innen; dann je Kante P→Q: innen/innen → Q; innen/außen → S;
außen/innen → S, Q; außen/außen → nichts; zuletzt fällt das doppelte P0 weg.

### 7.1 Nah (0xd7308, Texturfamilie; Ebene z = 0, innen = z ≤ 0)

```c
/* I = innere Ecke, O = äußere, Basis der Interpolation ist immer I */
num = 0 - I.z   /* gepatchtes 'add ecx,0' bei 0xd7470: 0 */;  den = O.z - I.z;
x' = (int32)(((int64)(O.x - I.x) * num) / den) + I.x;     /* idiv: zur Null */
y' = (int32)(((int64)(O.y - I.y) * num) / den) + I.y;
sy = (int16)((y' << 7) / [0x829ac]) + 130;   /* /(-128): = 130 - y' */
sx = (int16)((x' << 7) / [0x829ac]) + 159;   /* = 159 - x' */
/* 16-Bit-Addition läuft über -> Wert 0x7fff und Polygon verworfen (0xd7a1c = -1) */
U(c) = (raw_u(c) * mat[+8]) >> 5;            /* wie im Setup */
u' = (int16)((((int64)(((raw_u(O) - raw_u(I)) * mat[+8]) >> 5)) * num) / den) + U(I);
v' entsprechend mit raw_v und mat[+0xc];
```

Die Schnittpunkte liegen bei z = 0, ihre Projektion ist also exakt `159 − x'`, `130 − y'` (wie
PERSP[0] = −65536). Die Ecken innen kommen mit ihrem SCR-Wert und ihren skalierten u, v.

### 7.2 Fern (0xd76a0; innen = z ≥ FAR)

Wie 7.1 mit `num = FAR − I.z` (≤ 0, über ds:0xd7a24 = −FAR), `den = O.z − I.z` (< 0) und

```c
sy = (int16)((y' << 7) / [0x829b0]) + 130;   /* 0x829b0 = FAR - 128, im RAM -43936 */
sx = (int16)((x' << 7) / [0x829b0]) + 159;
/* Überlauf -> 0x7fff, aber KEIN Verwerfen */
```

Achtung: hier zur Null gerundet, im Tabellenweg abgerundet.

FAR kommt aus der Detail-Einstellung über 0x8baa4 (schreibt 0xd7a20, 0xd7a24 = −FAR,
0x829b0 = FAR − 128 und die Immediates der Klasse FERN); laut `firmware/track.c` Tabelle 0x21d4:
−27808, −27808, −43808 (im RAM: −43808).

Andere Familien (Tabelle 8.1): flache Farbe 0xd80a0 / 0xd8280 (Puffer mit Farbwort, Ecken nur
`scr`, 4 Byte), Reflexion 0xda500 / 0xda844 (u, v aus dem Reflexionspuffer, **mit**
Materialgröße skaliert), 192/208 0xca6e0 / 0xd8280. Nicht im Einzelnen gelesen (**offen**).

## 8. Übergabe an die Füllroutinen

### 8.1 Liste, Verteiler, Routinentabelle

Nach den Flächen eines Blocks zeichnet 0x840fc die Liste 0x5d188 in ihrer Reihenfolge:

```c
for (each {w, p} until w == -1) {
    mat = MLB + (w & ~0xf);                 /* MLB = [ds:0x121540] */
    call [ mat[+0] + (w & 0xf) + 0x822e0 ](p);
}
```

`mat[+0]` (Routinennummer, Vielfaches von 16) wählt eine Zeile zu vier Einsprüngen:
+0 Fläche, +4 geclipptes Polygon, +8 Nah-Clipper, +0xc Fern-Clipper. Im RAM:

| Nummer | +0 Fläche | +4 geclippt | +8 nah | +0xc fern | Bedeutung (NOTES) |
|---|---|---|---|---|---|
| 0, 16, 32 | d5350 | d51e0 | d7308 | d76a0 | Textur |
| 48 … 112 | dc1b4 | dc078 | d7308 | d76a0 | Textur, Texel 0 durchsichtig (64) |
| 128, 144 | d8844 | d8744 | d80a0 | d8280 | eine Farbe (Farbe = face+2) |
| 160, 176 | db310 | db1c0 | da500 | da844 | Reflexion |
| 192, 208 | cae84 | cad84 | ca6e0 | d8280 | Farbe aus face+2, Art **offen** |
| 224, 240 | cd324 | cd1e8 | d7308 | d76a0 | durchsichtig über MAPn.SMG |

Auf Strecke 1 laut NOTES: 16, 64, 128, 160 (ein Material), 224.

### 8.2 Setup Textur (0xd5350 Fläche, 0xd51e0 geclippt)

```c
mat = MLB + face[+4]  -> ds:0x84270;   n = face[+6] -> ds:0x8b564;
for (i = 0; i < n; i++) {                       /* Ecken face+8, +0xa, +0xc, (+0xe) */
    o = face[+8 + 2*i];
    poly[i].scr = SCR[o];                       /* int16 sy, int16 sx */
    poly[i].u = (int16)((uv[o].u * mat[+8])  >> 5);   /* 1/8 Texel: raw*Breite/256*8, abgerundet */
    poly[i].v = (int16)((uv[o].v * mat[+0xc]) >> 5);
}
poly[n..2n-1] = poly[0..n-1];                    /* Puffer 0xd7a60, zweimal hintereinander */
```

224 (cd324) nimmt bei Breite und Höhe 256 `raw << 3` (gleiches Ergebnis), sonst dasselbe.
Dann Annahme/Verwerfung (beide Einsprünge gleich bis auf eine Kleinigkeit, s. u.) mit
min/max über die Ecken:

```c
verwerfen, wenn: eine von min_sy, max_sy, min_sx, max_sx <= -16000 oder > 16000;
                 min_sx == max_sx (keine Breite);
                 max_sy < 0;  max_sx < 0;  min_sx > 319;  min_sy > H;   /* H = Sichthöhe, 200 */
ganz innen, wenn: min_sy >= 0 && min_sx >= 0 && max_sy <= H && max_sx <= 319
    -> Füller ohne Rand-Clipping: 0xd55ff (beide Einsprünge, 0xd51e0 über 0xd5327);
sonst -> 2D-Clipper am Bildrand: 0xd4b60 (Fläche) bzw. 0xd52e0 → 0xd4b79 (geclippt).
```

Kleinigkeit: in 0xd5350 werden min_sx und max_sx als Doppelwort mit max_sy im unteren Wort
verglichen; dadurch fällt ein Polygon weg, dessen linkste Ecke genau auf Spalte 319 liegt und
dessen max_sy > 0 ist, und eines mit max_sx = 319 geht in den Clipper. 0xd51e0 vergleicht ohne
das untere Wort. H ist in den Code gepatcht (0xd65e0 aus ds:0x8a514).

Ab hier beginnt der Füller (anderes Dokument): Kanten über die Kehrwerttabelle 0x5b26c, Spans,
2D-Clipper mit 16-Bit-`idiv` ab 0xd4b60.

### 8.3 Texturadresse und Bild (im Füller, 0xd5980)

```c
tex = MLB + mat[+4];   frames = tex[+0];
bild = (frames * phase) >> 14;                     /* phase = ds:0x84284, Abschnitt 4.2 */
pixel = MLB + tex[+8 + 4*bild];                    /* Zeilen zu 256 Byte */
/* Reflexion (0xdb8c8): bild = (frames * refl) >> 16, refl = rec[+0x3c] */
```

### 8.4 Flache Farbe (128, 0xd8844)

Farbe = `(int16)face[+2]` → ds:0xda4d4; Ecken nur `scr` (4 Byte, Puffer 0x5c9b8, doppelt), sonst
dieselbe min/max-Prüfung.

### 8.5 Reflexion (160): u, v pro Vertex in Gleitkomma (0xc1fc0, 0xc2230)

Nur Blöcke mit Flag 0x2000 (auf allen Strecken die 49 Blöcke mit Vertex-Normalen, vor allem
Autos). Vor den Flächen, pro Vertex i (Normalen bei `[blk+0x20]`: int32 Anzahl, dann float x, y, z):

```c
n  = (n_obj * F[0..8]) * 6.1035e-5f;               /* in den Sicht-Raum, float */
p  = VIEW[i] (int -> float);  l = sqrt(p.p);
e  = p / l;                                        /* als float32 gespeichert */
dd = 2 * (e.n);   r = dd*n - e;
u  = fistp(127 * r.x) + 127 - offx;                /* fistp: auf Gerade runden */
v  = fistp(127 * r.z) + 127 - offz;                /* r.z, nicht r.y */
REFL[off] = (v << 16) | (u & 0xffff);              /* 0xc3bd4, Index = Vertex-Byteoffset */
```

`offx`, `offz` = `& 0xff` zweier Float-Akkumulatoren im Objekt: 0xc1fc0 nimmt, nur bei einem
bewegten Objekt (Zeiger `[0x7427c]` mit Bit 30), den Weg seit dem letzten Zeichnen (Lage −
rec+0x24, ganze Einheiten, anfangs 0) als float, dreht ihn mit 0x8aab0, mal dem Faktor an
**0xc2428** (eine Konstante im Code), und addiert ihn auf rec+0x30, +0x34, +0x38; `fist` (auf
nächste ganze Zahl) `& 0xff` der ersten und dritten Summe sind die Versätze; sonst 0. Der Faktor
steht im Programm auf 0,008; **0x1813c** (Fahrphysik des Spielerautos, nur im Zustand +0x148 = 0,
also nicht im Unfall) setzt ihn jedes Bild nach der Art des zuletzt erreichten Markers
(Auto +0x66c = `MAPn.MRK` +4, geschrieben von 0x1820c): Art 0 → 0,002 (`0x3b03126f`), Art 1
und 2 → 0,03 (`0x3cf5c28f`), andere lassen ihn. Er gilt für alle Objekte.

**Das Reflexionsbild:** 0x1813c setzt im selben Zug Auto +0x7b0 = 0 / 0x8000 / 0xff00 für Art
0 / 1 / 2; 0x1607c kopiert es jedes Bild in den Objektsatz (rec+0x3c), 0x8aafc nach [0x84288],
und der Füller nimmt Bild `(frames × [0x84288]) >> 16` des Texturdatensatzes (0xdb8c8). Auf
Strecke 1 hat die Textur des 160-Materials drei Bilder (254 × 254): 0 der Himmel, 1 (Art 1,
die Marker 3 und 4: der Tunnel) die Lichter der Tunneldecke, 2 ein drittes. Gegner rufen
0x1813c nicht auf (ihr +0x7b0 bleibt 0 aus 0x14095): nur das Spielerauto spiegelt den Tunnel.

Das Setup 0xdb310 nimmt `u = REFL.u << 3` (ohne Materialgröße), der Nah-Clipper 0xda500
dagegen `(REFL.u * mat[+8]) >> 5` — ein Unterschied im Original. Die Texeladresse wird
`& 0xffff` genommen (0xdb59b): u, v laufen über 256 hinaus herum. Die x87-Zwischenwerte laufen
in 80 Bit mit einzelnen float32-Speicherungen. Dass v aus r.z kommt, steht so im Code (NOTES
sagt „seitlich, oben“).

**Am RAM geprüft (2026-10-30, DOSBox, Startaufstellung Strecke 1, nahe Verfolgerkamera):**
Objektsatz des Spielers (Mesh 229) mit rec+0x24 = (2600, 0, −18500), Akkumulatoren (−5,086755,
−0,209960, 37,013203), also offx = 251, offz = 37; rec+0x3c = 0; Faktor an 0xc2428 = 0,002 (die
Fahrphysik läuft schon in der Startaufstellung). Die Tabelle 0xc3bd4 (6 Byte je Vertex, der
Index ist der Vertex-Byteoffset) hält danach die 128 Werte der Karosserie des Spielers; unsere
Geometrie-Einheit mit denselben Akkumulatoren: 95 von 128 Vertices gleich, die übrigen um eins
(35 von 256 Werten), die Gleitkomma-Form ebenso — der Rest kommt aus den Eingängen
(Gleitkomma-Kamera und Sichtraum mit 8 Nachkommabits statt der ganzzahligen VIEW).

## 9. Reihenfolge in einem Bild (0x81870)

1. 0x83a68 Hintergrund.
2. 0x83114: Zellenfenster, Liste der Stücke in Reichweite (2.1, 2.2); 0x8a6a8 hängt die bewegten
   Objekte an (2.3).
3. 0x8a7b0: Sicht-Raum-Position, Schlüssel, Sichtbarkeit (3); 0x832c0 sortiert: fern zuerst.
4. Pro Objekt in dieser Reihenfolge: 0x8ac58 rechnet Matrizen, Schlüssel und Detailstufe aller
   Blöcke und sortiert die Blöcke (fern zuerst); dann pro Block in dieser Reihenfolge:
   0x8a628 (F einsetzen, Block ohne Stufe überspringen), 0x83380 (alle Vertices), 0x83468
   (Flächen in Zeilenreihenfolge, Clipper sofort an Ort und Stelle), 0x840fc (Polygone füllen,
   Listenreihenfolge).

Reiner Maler-Algorithmus ohne Tiefenpuffer: Objekte fern → nah, Blöcke eines Objekts fern → nah,
Flächen eines Blocks in der vorberechneten Zeilenreihenfolge.

## 10. Für kleine Hardware

- **Alles bis F und G ist CPU-Arbeit pro Block**, ganzzahlig und billig (Sinustabelle aus der EXE,
  9-fach-Produkte, `>> 14`). Die Hardware braucht pro Vertex 9 Multiplikationen 16×16 → 32
  (Matrix ≤ 16384, Vertex int16; die Summe läuft nie über), drei Addierer, einen Schieber, dann
  `e = −(2^23 / (128 − z))` (Tabelle mit 64K Einträgen oder ein Divisor mit 17-Bit-Ergebnis,
  beides bitgleich) und zwei Multiplikationen 32×17 → 32 mit `>> 16`.
- Der Rückseitentest ist ein Skalarprodukt (int16-Differenz × int32-Normale, 32 Bit) in
  Blockkoordinaten, die Flächenreihenfolge kommt fertig aus den Daten: kein Sortieren von Flächen.
- Sortiert werden nur Objekte (≈ 150) und Blöcke (≤ 12), nach 16-Bit-Schlüssel.
- Wurzeln (Schlüssel, Detailstufe) sind `round(sqrt(n))` einer Ganzzahl: `isqrt` plus
  `if (n − r² > r) r++` ist exakt.
- Clipping betrifft nur Blöcke an der Kamera (z + r ≥ −8) oder an FAR. Nah: Schnittpunkte
  brauchen keine Projektionsdivision (`159 − x'`, `130 − y'`), aber vier echte Divisionen
  64/32 (x, y, u, v), zur Null gerundet. Fern: zusätzlich zwei Divisionen durch FAR − 128.
- Gleitkomma bleibt nur bei der Umrechnung der Kamerawinkel (einmal pro Bild, CPU) und beim
  Reflexionspfad (160, nur Autos).
- Tabellen: PERSP01.TAB (256 KB, ersetzbar), VIS.TAB (157 KB, nur Klasse NAH, nicht ersetzbar).

## 11. Was die heutige Portierung (`firmware/track.c`) anders macht

Aus dem Vergleich, nicht vollständig: Gleitkomma-Matrizen statt der Tabellenmatrizen mit
16383-Maßstab und Rundung pro Verkettung; exakte Inverse statt `0x7ff − a` (das Bild des
Originals ist um je 1/2048 Umdrehung in allen drei Winkeln verdreht); 1/16 Pixel statt
`floor(x·e/65536)` mit abgeschnittenem `e`; **immer Detailstufe 0** (`block_of()` liest nur
mesh+0x18), das Original zeichnet ferne Blöcke aus Stufe 1 und 2; Frustumtests mit Kugeln statt
des Halbauflösungstests aus 3; kein VIS.TAB-Test; Schnittpunkte am Clipper anders gerundet;
Rückseitentest mit dem Projektionsauge statt mit E12 (6.2: das Original lässt die vom
Projektionsauge abgewandten Flächen im Füller weg; was E12 zusätzlich verwirft, zeichnet die
Portierung, in den geprüften Bildern ohne ein anderes Pixel).

## 12. Offen

- Ob die VIS.TAB-Verwerfung (6.3) je sichtbare Flächen trifft, und eine Formel für VIS.TAB.
- Routinen 192/208 (cae84): was sie tun, und ob sie auf einer Strecke vorkommen.
- Die Clipper der Familien flach, Reflexion, 192 im Einzelnen (7, letzter Absatz).
- Der Reflexionspfad (8.5): woher die Akkumulatoren eines Autos in der Startaufstellung ihren
  Stand haben (das erste Zeichnen addiert die ganze Lage, gedreht mit der Kamera dieses
  Augenblicks, während des Kameraschwenks), ist ohne die Vorgeschichte nicht nachzurechnen. Die
  Konstanten 0xc242c (9,99) und 0xc2430 (1024, ein toter Vergleich) sind unbenutzt.
- P+0x1f0 (gerundeter Abstand des Blockursprungs, in float32 summiert, 0x83487) wird geschrieben,
  ein Leser ist nicht gefunden; ebenso ds:0x8b554 = 0x17700000 pro Block.
- Die Sichteinstellungen (0x8a510..0x8a53c) für andere Ansichten als die 200 Zeilen hohe (Cockpit:
  132) sind nicht ausgelesen.
- Der 2D-Clipper am Bildrand (0xd4b60) gehört zum Füller und ist hier nicht beschrieben.
- Wo die Zellenbox (Lauf 4 einer MMP-Zelle) herkommt und ob jede Zelle eine hat.

# Die Polygonfüller des Originals (STARTL.EXE)

Stand 2026-10-29. Aus dem Code gelesen (`tools/dis.sh -f`, Adressen flach wie in openscreamer
`docs/NOTES.md`: Code ab 0x10000, Daten ab 0x100000) und, wo der Code sich selbst ändert oder
Tabellen erst zur Laufzeit entstehen, aus dem Speicher des laufenden Originals
(`tools/dosram.py`, Strecke 1, Rennen auf dem Startplatz, pausiert). Diese Datei beginnt am
Eingang des Füllers; wie die Ecken auf ganze Pixel projiziert werden, beschreibt die
Geometrie-Doku. Alles, was nicht belegt ist, steht unter **offen**.

## Kurz

- **Spalten, nicht Zeilen.** Jeder Füller läuft die Polygonkanten **in x** ab (eine Kante je
  Pixelspalte, x ganzzahlig, keine Subpixel) und zeichnet dann **senkrechte Spannen**, Spalte
  für Spalte von rechts (maxX) nach links (minX), in jeder Spalte von oben nach unten.
- **Affin, ohne jede Perspektive.** u, v werden entlang der oberen und unteren Kante linear in
  x interpoliert und in der Spalte linear in y. Keine Division je Pixel, keine Unterteilung,
  kein Tiefenpuffer, kein Licht, kein Dithering.
- **Zwei Kehrwerttabellen ersetzen jede Division:** `T1[dx]` (≈ 65536/dx) für die Kanten,
  `T2[n]` = ⌊32767/n⌋ für die Spalten. Beide runden ab, `T1` hat zusätzlich einen Fehler bei
  jedem Vielfachen von 11.
- **Ganze Eckkoordinaten:** Eckpunkt = (x, y) ganzzahlig, gepackt als `(x << 16) | (y & 0xffff)`;
  u, v je Ecke in Achteltexeln (3 Nachkommabits).
- Eine Spalte deckt die Zeilen `⌊yo⌋ … ⌊yu⌋` **einschließlich** (Routinen 16, 64, 160; die
  letzte Zeile wiederholt das Texel der vorletzten), Routine 224 die Zeilen `⌊yo⌋ … ⌊yu⌋−1`.
  Spalten mit `⌊yu⌋ − ⌊yo⌋ ≤ 0` bleiben leer (nur die Einfarbroutine 128 zeichnet dort 1 Pixel).
- Textur: 256 Byte je Zeile, `texel = data[(v & 255) * 256 + (u & 255)]`, u und v laufen modulo
  256 um (kein Begrenzen auf die Materialgröße).

## 1. Verteiler und die Routinen von Strecke 1

### Routinentabelle `0x822e0`

Ein Material (16 Byte in `MAPn.MLB`, Basis `[0x121540]`) beginnt mit seiner Routinennummer
`R`. Gerufen wird `[0x822e0 + R + k]`, `k` = Variante:

| `k` | wann | Eingang |
|---|---|---|
| 0 | Fläche ganz vor der Nahebene und vor der Fernebene | Zeiger auf den Flächen-Record |
| 4 | Polygon, das ein Klipper (k = 8 oder 12) schon als Eckliste gebaut hat | Zeiger auf die Eckliste |
| 8 | Fläche schneidet die Nahebene (z = 0) | Flächen-Record (ebp), klippt in 3D |
| 12 | Fläche schneidet die Fernebene | Flächen-Record (ebp), klippt in 3D |

Die Routinen, die Strecke 1 benutzt (aus der Materialtabelle im RAM: 251 × 16, 91 × 64,
9 × 128, 2 × 224, 1 × 160):

| R | was | k=0 | k=4 | k=8 | k=12 |
|---|---|---|---|---|---|
| 16 | Textur deckend | `0xd5350` | `0xd51e0` | `0xd7308` | `0xd76a0` |
| 64 | Textur, Texel 0 durchsichtig | `0xdc1b4` | `0xdc078` | `0xd7308` | `0xd76a0` |
| 224 | Mischung über `MAPn.SMG` | `0xcd324` | `0xcd1e8` | `0xd7308` | `0xd76a0` |
| 128 | eine Farbe, keine Textur | `0xd8844` | `0xd8744` | `0xd80a0` | `0xd8280` |
| 160 | Spiegelung (u, v aus `0xc3bd4`) | `0xdb310` | `0xdb1c0` | `0xda500` | `0xda844` |

(Die Tabelle hat weitere Einträge, z. B. R = 48 = wie 64 und R = 192 = `0xcae84`…, die auf
Strecke 1 nicht vorkommen.)

### Zeichenliste

Die Flächenschleifen (`0x83680` mit Nahtest, `0x83940` mit Ferntest) schreiben je sichtbare
Fläche ein Paar `(Materialoffset | k, Zeiger)` in eine Liste und klassifizieren dann die vier
Ecken (bei Dreiecken ist Ecke 3 = Ecke 0) nach der Kamera-Tiefe `fs:[ecke*2]`:

```
Nahtest (0x83878):  alle z >= 0  -> Paar wieder entfernen (ganz hinter der Kamera)
                    gemischt     -> sofort [0x822e8 + R] (k = 8) rufen; der Klipper
                                    ersetzt das Paar durch (Material+4, Eckliste)
                    alle z < 0   -> Paar bleibt (k = 0)
Ferntest (0x839af): alle z <= zf -> entfernen; gemischt -> k = 12; sonst bleibt es.
                    zf = 0xffff54e0 = -43808 (DETAIL HI, in den Code geschrieben)
```

Die Liste endet mit `-1`. Ausgeführt wird sie in **Listenreihenfolge** von `0x84100`
(Malerverfahren, kein z):

```
loop: m = *list++; if (m == -1) return;  p = *list++;
      cld; call [0x822e0 + mlb[m & ~15].routine + (m & 15)] (eax = p)
```

### Texturbild und Bildspeicher

```
mat   = MLB + materialoffset                 (Flächen-Record +0x04, int16)
rec   = MLB + mat[+4]                        (Texture-Record)
frame = (rec[0] * [0x84284]) >> 14           (R = 16, 64, 224)
frame = (rec[0] * [0x84288]) >> 16           (R = 160)
tex   = MLB + rec[8 + 4*frame]               (in 4 Ladebefehle der Spaltenschleife geschrieben)
pixel(x, y) = [0x121524] + rowtab[y] + x,    rowtab = ds:0x20bb8 (= 320*y, Eintrag 200 = 64000)
```

Im RAM bestätigt: das in `0xd5a53` eingeschriebene Texturbild des zuletzt gezeichneten Polygons
ist genau das so berechnete (Material 213, 8 Bilder, Bild 3).

## 2. Eingabe der Füller

### Ecke

Projizierte Ecke `P` = Dword aus `0x64e94 + eckoffset` (eckoffset = Wort aus dem
Flächen-Record `+0x08…`, Byte-Offset der Ecke):

```
P = (x << 16) | (y & 0xffff)      x, y ganze Pixel (vorzeichenbehaftete 16 Bit)
```

### u, v je Ecke (Routinen 16, 64, 224)

```
raw_u, raw_v = int16 aus [0x841c4] + eckoffset, +2          (Block +0x24, 0..255)
U8 = (int16)((raw_u * mat.extU) >> 5)                         (sar, 32 Bit, unten 16 Bit behalten)
V8 = (int16)((raw_v * mat.extV) >> 5)
```

`U8`, `V8` sind Texel × 8. Bei `extU = extV = 256` nimmt Routine 224 einen kürzeren Weg
(`raw << 3`), der dieselben 16 Bit ergibt. Die Eckliste in `0xd7a60` hat je Ecke 8 Byte
`[P][U8 | V8 << 16]` und wird für den Kantenlauf einmal hinten angehängt (Ecke n…2n−1 =
Ecke 0…n−1). Nur 3 oder 4 Ecken (Flächen-Record `+0x06`).

**Routine 160:** `raw` aus der Tabelle `0xc3bd4` (pro Bild von `0xc2230` berechnet, siehe
NOTES.md), `U8 = (raw_u << 3) & 0xffff`, `V8` ebenso — **ohne** die Materialgröße (das einzige
160-Material hat 254 × 254). Der Nahklipper `0xda500` dagegen rechnet mit `raw * ext >> 5`.

### Farbe (Routine 128)

`farbe = (uint8)Flächen-Record[+0x02]` (k = 0) bzw. das dritte Dword der Eckliste (k = 4);
Ecken nur `P` (4 Byte je Ecke, Liste `0x5c9b8`).

### Eckliste aus den 3D-Klippern (k = 4)

`[Materialoffset][n][P0][UV0]…[Pn-1][UVn-1]` (bei 128: `[Material][n][Farbe][P0]…`), in einem
Puffer `[0x792ac]`.

## 3. 3D-Klippen an Nah- und Fernebene (k = 8, 12; `0xd7308`, `0xd76a0`)

Sutherland-Hodgman über die Ecken in Flächenreihenfolge. Kamera-Koordinaten: `z = fs:[o*2]`,
`x = gs:[o*2]`, `y = gs:[o*2+4]` (o = Eckoffset). Innen ist bei der Nahebene `z <= 0`, bei der
Fernebene `z >= zf` (`[0xd7a20]` = −43808). Ecken, die innen liegen, kommen unverändert mit
ihrem `P` aus `0x64e94`. Schnittpunkt (in beiden Richtungen von der **inneren** Ecke `i` aus,
die äußere heißt `a`; 32×32→64-Bit-Produkt, `idiv` schneidet zur Null hin ab):

```
num = -(z_i + E)          E = 0 (Nah), 43808 = [0xd7a24] (Fern)
den = z_a - z_i
yc  = (y_a - y_i) * num / den + y_i
xc  = (x_a - x_i) * num / den + x_i
Py  = (int16)( (yc << 7) / D + [0x8a52c] )     D = [0x829ac] = -128 (Nah), [0x829b0] = -43936 (Fern)
Px  = (int16)( (xc << 7) / D + 159 )           [0x8a52c] = 130
U8c = (((int16)(raw_u_a - raw_u_i) * extU) >> 5) * num / den + ((raw_u_i * extU) >> 5)
V8c = ebenso mit v / extV
```

Läuft die 16-Bit-Addition über (Vorzeichen), wird der Wert 0x7fff; beim **Nah**klipper wird das
Polygon dann verworfen (`[0xd7a1c] = -1`), beim Fernklipper nicht. Weniger als 3 Ecken:
verworfen. Hinweis: der Nahtest der Flächenschleife zählt `z = 0` als „hinter“, der Klipper als
„innen“. (Die Projektion `x·128/(z−128)+159` passt zu D = −128 bei z = 0 und −43936 bei
z = −43808; das gehört zur Geometrie-Doku.)

## 4. Vorprüfung und 2D-Klippen

### Grenzen der Ecken

Über alle Ecken (Routine 16, k = 0; die anderen gleichwertig):

```
imin = Index des kleinsten P (signed 32 Bit, also kleinstes x, dann kleinstes y);
       bei Gleichstand gewinnt die SPÄTERE Ecke
minY, maxY (int16), minX, maxX
```

Verworfen (nichts gezeichnet), wenn:
- ein Grenzwert außerhalb (−16000, +16000] liegt (`0xc180`/`0x3e80`, bei x über das ganze Dword),
- `minX == maxX` (null Spalten breit),
- `maxY < 0` oder `maxX < 0`,
- `minY > B` (B = `[0x8a514]` = 200, beim Start in die Vergleiche geschrieben: `0xd65e0`,
  `0xdc8d0`, `0xdb9b0`, …),
- k = 0: `((minX << 16) | maxY) > (319 << 16)` — also `minX >= 320`, **oder** `minX == 319`
  und `maxY > 0`; k = 4: nur `minX > 319`.

Liegt alles in `0 <= x <= 319`, `0 <= y <= B` (genauer: `minY >= 0`, `minX >= 0`, `maxY <= B`,
`(maxX << 16 | maxY) <= 319 << 16`), geht es direkt zum Kantenlauf, sonst erst durch den
2D-Klipper.

### 2D-Klipper (`0xd4b60`; 64: `0xdbbe0`, 224: `0xccd50`, 160: `0xdaba4` — gleicher Code;
128: `0xd8444` ohne u, v)

Sutherland-Hodgman in vier Stufen, jede nur, wenn nötig:

| Stufe | wann | innen |
|---|---|---|
| x = 0 | `minX < 0` | `x >= 0` |
| y = 0 | `minY < 0` | `y >= 0` |
| x = 319 | `maxX >= 319` | `x <= 319` (Konstante, nicht `[0x8a510]`) |
| y = B | `maxY >= B` | `y <= B` (B = `[0x8a514]` = 200) |

Nach jeder Stufe: weniger als 3 Ecken → nichts. Schnittpunkt immer von der **äußeren** Ecke
`a` aus (darum liefern beide Richtungen denselben Punkt), alles in **16 Bit**: `imul r16`
(16×16→32), `idiv r16` (zur Null hin), Ergebnis die unteren 16 Bit. Beispiel Ebene x = c
(c = 0 oder 319), innere Ecke `i`:

```
y   = y_a + (int16)( (x_a - c) * (y_a - y_i) / (x_i - x_a) )   hier (x_a-c)*(y_a-y_i) als
                                                                  16x16-Produkt
u8  = U8_a + (int16)( (U8_i - U8_a) * (c - x_a) / (x_i - x_a) )
v8  = V8_a + (int16)( (V8_i - V8_a) * (c - x_a) / (x_i - x_a) )
P   = (c << 16) | (y & 0xffff)
```

Ebene y = c entsprechend mit vertauschten Rollen (`x = x_a + (x_i−x_a)·(c−y_a)/(y_i−y_a)`,
u, v mit `(c − y_a)/(y_i − y_a)`). Die Differenzen sind 16-Bit-Differenzen, die Produkte
passen in 32 Bit, der Quotient muss in 16 Bit passen (sonst #DE — bei |Koordinate| < 16000
nicht erreichbar). Danach wird `imin` aus der geklippten Liste neu bestimmt (gleiche Regel) und
derselbe Kantenlauf benutzt.

## 5. Kantenlauf (Routine 16: `0xd55ff`…; 64: `0xdc443`; 224: `0xcd5bf`; 160: `0xdb59b`;
128: `0xd8958`)

Zwei Ketten laufen von der Ecke `imin` los: **A vorwärts** (Ecke imin, imin+1, …) und
**B rückwärts** (imin, imin−1, …). Kanten mit `dx <= 0` (16-Bit-Differenz) werden in beiden
Ketten übersprungen. A ist die **obere**, B die **untere** Begrenzung: das stimmt nur, wenn die
Ecken auf dem Bildschirm (y nach unten) **im Uhrzeigersinn** laufen. Andersherum ist überall
`yu − yo <= 0` und das Polygon bleibt unsichtbar (zweites, implizites Rückseitenkriterium).

Je Kante (Start s, Ende e, `dx = x_e − x_s > 0`):

```
stepY = (int16)(y_e - y_s) * T1[dx]                  16.16, exaktes 32-Bit-Produkt
Y     = y_s << 16
Routine 16:       stepU = p >> 11, wenn p < 0 dann +1      p = (int16)(U8_e - U8_s) * T1[dx]
Routinen 64, 224: stepU = p >> 11                          (sar, abrunden)
                  U = (int16)U8_s << 5                      -> U, V sind 8.8 (Texel * 256)
Routine 160:      stepU = p >> 4,  U = (int16)U8_s << 12    -> 17.15 (Texel * 32768)
V entsprechend.   Routine 128 hat nur Y.
```

Der Lauf geht spaltenweise von `x = minX` bis `maxX`; für jede Spalte wird ein Eintrag
`(YB, YA, UB, VB, UA, VA)` gespeichert (Tabelle auf dem Stapel ab `0xba470` abwärts, 24 Byte
je Spalte; 128: `(YB, YA)` ab `0xba498`, 8 Byte, aufgerollte Schleife über Sprungtabelle
`0xd89f0`). Pro Spalte: Eintrag schreiben, dann jede Größe um ihren Schritt erhöhen (32 Bit,
Überlauf wie Hardware).

Ende einer Kante: Erreicht eine Kette ihre Endecke (gleiches x), wird die **Spalte dieser Ecke
neu geschrieben**: die Kette, deren Kante endet, beginnt die nächste Kante mit den **exakten
Eckwerten** (`Y = y << 16`, `U = U8 << 5`), die andere Kette behält ihren **aufaddierten** Wert.
Enden beide an derselben Spalte, beginnen beide neu (`0xd5623`). In der **letzten Spalte (maxX)**
stehen die aufaddierten Werte beider Ketten, keine Eckwerte. Ende, wenn beide Ketten an
derselben Ecke ankommen (Zeigervergleich `0xd5623`).

```
c   = x_s;  A = Kante(A); B = Kante(B)
loop:
  xe = min(A.xend, B.xend)
  for (; ; c++) { spalte[c] = (B.Y, A.Y, B.U, B.V, A.U, A.V);
                  if (c == xe) break;  A += A.step;  B += B.step; }
  if (fertig) break
  if (A.xend == xe) A = nächste Kante(A) mit Startwerten aus der Ecke   // überschreibt spalte[xe]
  if (B.xend == xe) B = nächste Kante(B) mit Startwerten aus der Ecke   // beim nächsten Durchlauf
```

Folgen der Rundung (bit-genau nachzubauen): `T1` rundet ab, darum bleibt eine **abwärts**
laufende Kante (dy > 0) am Kantenende knapp unter `y_e << 16`; ist das die aufaddierte Kette
(z. B. in Spalte maxX), endet sie eine Zeile zu früh. Bei dx = 1 ist `T1[1] = 65535`, also immer.

### Tabelle T1 (`0x5b26c`, fest im EXE, 704 Einträge, im RAM gleich)

```
T1[0] = T1[1] = 65535
T1[n] = 65536 / (n + 1)   wenn n % 11 == 0      (Fehler der Tabelle: 11 -> 5461 statt 5957)
T1[n] = 65536 / n         sonst                  (abgerundet), 2 <= n <= 703
```

Über alle 702 Einträge geprüft. dx ist nach dem Klippen höchstens 319.

## 6. Spalten zeichnen

Die Spalten werden von `x = maxX` abwärts bis `minX` gezeichnet. `yo = YA >> 16`,
`yu = YB >> 16` (arithmetisch, also abgerundet), `n = yu − yo`.

- Am rechten Ende werden Spalten übersprungen, solange `yo < 0` oder `yu < 0` oder `n <= 0`
  (`0xd5954`); danach nur noch bei `n <= 0` (`0xd5a00`). Ende an der Spalte minX bzw. am Wächter
  über der Tabelle (`(0, 0x08000000)`, d. h. `yo = 0x800`).
- Routinen 16, 64, 160: **n + 1 Pixel**, Zeilen `yo … yu`; die Zeile `yu` bekommt dasselbe Texel
  wie die Zeile `yu − 1` (`0xd5aca`). Routine 224: **n Pixel**, Zeilen `yo … yu−1`.
- Routine 128: erste gezeichnete Spalte braucht `n > 0`, danach wird jede Spalte mit `n >= 0`
  gezeichnet, **n + 1 Pixel** in der Farbe (also 1 Pixel bei n = 0).
- Zeile B = 200 kann beschrieben werden (Klippen bei y <= 200 einschließlich, Zeilentabelle hat
  Eintrag 200). Sie liegt hinter dem sichtbaren 320 × 200-Bild.

### Texturschritt in der Spalte, Routinen 16, 64, 224 (`0xd5a0e`…)

Ein Teiler je Spalte über `T2`, u mit 16 Nachkommabits, v mit 8, in zwei Registern gepackt:

```
U0 = UA, V0 = VA                  Routinen 64, 224
U0 = UA & ~15, V0 = VA & ~15      Routine 16 (1/16-Texel-Raster)
pu = (int32)((UB - U0) * T2[n])           // 32 Bit, Überlauf wie Hardware
r  = rol32(pu, 9)
cu = r & 0xff                              // ganzzahliger u-Schritt (mod 256) = pu[30:23]
pv = (int32)((VB - V0) * T2[n]) >> 15      // 8.8, abgerundet
S  = (r & 0xffff0000) | (pv & 0xffff)      // [u-Bruch 16 Bit = pu[22:7]][v-Schritt 8.8]
E  = ((U0 & 0xff) << 24) | (V0 & 0xffff)   // [u-Bruch 8 Bit][0][v 8.8]
ui = (U0 >> 8) & 0xff
for (i = 0; i < n; i++) {
    t = tex[((E >> 8) & 0xff) * 256 + ui];          // v = E[15:8], u = ui, beide mod 256
    schreibe(x, yo + i, t);
    s = (uint64)E + S;  E = (uint32)s;
    ui = (ui + cu + (s >> 32)) & 0xff;              // Übertrag aus dem u-Bruch
}
if (R != 224) schreibe(x, yo + n, t);               // letztes Texel wiederholt
```

Eigenheiten: der u-Schritt endet nie genau bei UB (`T2[n]` = ⌊32767/n⌋ < 32768/n); der Übertrag
von v (Bit 15 → Bit 16) landet im untersten Bit des u-Bruchs (bei negativem v-Schritt fast jedes
Pixel, Größenordnung 2⁻¹⁶ Texel). Gerechnet wird mit dem Zustand **vor** dem Schritt.

### Routine 160 (`0xdb928`…)

```
du = (UB - UA) >> 7;  dv = (VB - VA) >> 7;  ua = UA >> 7;  va = VA >> 7   // 8.8
us = du / n;  vs = dv / n                    // idiv, zur Null hin
cu = (us >> 8) & 0xff
S  = ((us & 0xff) << 24) | (vs & 0xffff)     // Bits 23..16 = 0
E  = ((ua & 0xff) << 24) | (va & 0xffff)     // Bits 23..16 = 0
ui = (ua >> 8) & 0xff
Schleife wie oben (n Pixel, dann das letzte Texel noch einmal).
```

Hier hat u nur 8 Nachkommabits; die v-Überträge sammeln sich in Bits 23..16 und erreichen u in
einer Spalte (≤ 201 Pixel) nie.

### Tabelle T2 (`0xd5af0`, zur Laufzeit erzeugt)

`T2[n] = 32767 / n` (abgerundet) für 1 ≤ n ≤ 699, `T2[0] = 0x80000000` (nie benutzt) — im RAM
für alle Einträge geprüft.

## 7. Was ein Pixel ausmacht

| R | Pixel |
|---|---|
| 16 | `t` |
| 64 | `t`, aber nichts schreiben, wenn `t == 0` (auch beim wiederholten letzten Pixel) |
| 224 | `SMG[t * 256 + bildschirm]` (Segment `fs = [0x12149c]`; `t` wird nicht begrenzt, Tabelle hat 64 Zeilen) |
| 128 | `farbe` |
| 160 | `t` |

Sonst nichts: keine Schattierung, kein Dithering, kein z innerhalb des Polygons (Spalten und
Zeilen eines Polygons überlappen nicht), kein Bezug zum Himmel — der Hintergrund liegt schon im
Bildspeicher und wird übermalt. Gemeinsame Kanten benachbarter Polygone werden von beiden
gezeichnet (Spalte x_e gehört zu beiden, Zeile ⌊yu⌋ des oberen ist ⌊yo⌋ des unteren); es gilt,
wer später in der Liste steht.

## 8. Unterschiede der Routinen auf einen Blick

| | 16 | 64 | 224 | 160 | 128 |
|---|---|---|---|---|---|
| Kanten-u/v | `>>11`, +1 wenn negativ | `>>11` | `>>11` | `>>4` (17.15) | – |
| Spaltenanfang | `& ~15` | – | – | `>>7` | – |
| Spalten-u-Schritt | `T2`, 16 Bit Bruch | wie 16 | wie 16 | `idiv`, 8 Bit Bruch | – |
| Pixel je Spalte | n+1 | n+1 | n | n+1 | n+1 (auch n = 0) |
| Bild | `>>14`, `[0x84284]` | wie 16 | wie 16 | `>>16`, `[0x84288]` | – |

## 9. Was für die Hardware daraus folgt

Gegenüber unserem Rasterer (1/16-Pixel-Ecken, perspektivisch je Pixel) braucht der
Original-Füller: ganze Ecken, keine Subpixel-Korrektur, keine Kanten-Funktionen, keinen
Kehrwert je Pixel, keinen Tiefenpuffer. Je Kante ein Tabellenzugriff `T1[dx]` (704 × 16 Bit)
und drei Multiplikationen; je Spalte ein Zugriff `T2[n]` (≤ 201 × 15 Bit) und zwei
Multiplikationen; je Pixel nur zwei Additionen (32 Bit + 8 Bit mit Übertrag) und ein
Texturzugriff mit 16-Bit-Adresse `v·256 + u`. Spaltenweise Ausgabe heißt: der Bildspeicher wird
senkrecht beschrieben (Schrittweite 320).

## Offen

- **Gegen den Bildspeicher nur teilweise geprüft:** das letzte Dreieck des Bildes (Routine 16,
  Ecken (97,154), (124,153), (126,158)) mit dem Modell oben nachgerechnet: alle 96 Pixel liegen
  an der richtigen Stelle mit demselben Texelmuster, aber im Bildspeicher (`0xdc30`) stehen
  andere Farbnummern, durchgehend nach einer festen Zuordnung (70→67, 69→66, 71→67, 72→68,
  74→83, 90→52). Woher diese Umfärbung kommt (Pausenbild? ein späterer Durchgang?), ist offen;
  eine passende `SMG`-Zeile war im RAM nicht zu finden.
- Wer `T2` (`0xd5af0`) und die Wächterpaare `(0, 0x08000000)` über `0xba488` schreibt, ist nicht
  gefunden (Werte aus dem RAM).
- Was in Zeile 200 des Bildspeichers liegt und ob sie je sichtbar wird.
- Nicht konvexe Polygone und Ecken mit gleichem P: der Lauf setzt konvexe Polygone voraus; was
  das Original bei anderen tut, ist nicht untersucht.
- Die Flächen-Vorauswahl gegen die Seitenebenen (`0x836d7`…, Tabelle `0xb3218`) und welche der
  beiden Flächenschleifen ein Objekt bekommt: Geometrie-Doku.
- `0xdb9d0` (ein weiterer Klipper, liest vom Stapel) wird von der Routinentabelle nicht
  erreicht; wofür er da ist, ist offen.

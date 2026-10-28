# CPU-System (Schritt 1: die CPU zeichnet; Schritt 2: die Speicher)

Vertrag zwischen Hardware (`core/src/fpga/core/soc.v`) und Firmware (`firmware/`). Wer
etwas daran ändert, ändert beide Seiten und dieses Dokument.

## Überblick

- **CPU:** VexRiscv `VexPocketIMFC` (RV32IM + F + CfuPlugin, 8 KB I-Cache, 8 KB D-Cache
  write-through, keine MMU), erzeugt mit `tools/vexriscv/gen.sh`.
- **Takt:** Alles läuft auf `clk_74a` (74,25 MHz), ohne Taktübergang zur Bridge; auch der
  SDRAM-Controller. Nur `dram_clk` kommt aus einer eigenen PLL mit verstellbarer Phase
  (`docs/speicher.md`). Ein höherer Systemtakt ist ein späterer eigener Schritt.
- **Reset:** Die CPU steht im Reset, solange `reset_n` aus `core_bridge_cmd` low ist. Der
  Pocket lädt die Data-Slots ohne `deferload` vor „Reset Exit“, die Firmware liegt beim Start
  also schon im Boot-RAM. Beim Neuladen der Firmware aus dem Menü setzt der Pocket den Core zurück
  (Parameter-Bit 6), lädt aber keinen neuen Bitstream. Nach jedem Reset ist Puffer 0 vorn,
  kein Tausch steht aus und `SYS_SIM` ist 0. Bis zum ersten Tausch zeigt `video_out` Puffer 0
  mit Palette 0: nach dem Laden des Cores schwarz, nach einem Neuladen der Firmware, was der
  vorige Lauf dort hinterlassen hat.
- **Firmware:** zwei Teile. Data-Slot 0 `firmware.bin` (höchstens 32 KB) an Bridge-Adresse
  `0x00000000` ins Boot-RAM: der Lader (Selbsttests, Speichertest). Data-Slot 2
  `program.bin` an `0x1300_0000` ins SDRAM: das Programm.
- **Slots im SDRAM erst nach der Phasensuche:** Die Slots 2 (`program.bin`) und 3 (Strecke)
  haben `deferload`, der Pocket lädt sie nicht von selbst. Die Phasensuche des Speichertests
  stellt den SDRAM auch in schlechte Phasen, dort kann der Chip Befehle und Adressen falsch
  übernehmen und irgendwo schreiben (gemessen: 15 von 63 MB verändert). Der Lader fordert
  `program.bin` deshalb erst nach den SDRAM-Tests an (`DS_*`, der Ziel-Befehl 0x0180 von
  `core_bridge_cmd`), zuerst die 16 Bytes des Kopfes, in dem die Größe steht, dann die Datei;
  das Programm holt die Strecke genauso (`firmware/slot.c`). Die Bridge liefert 32-Bit-Wörter
  big-endian (das erste Byte der Datei in Bit 31:24), die CPU ist little-endian: `soc.v`
  dreht die Byte-Reihenfolge beim Schreiben um, beide sind ganz normale little-endian-Abbilder.
- **Start des Programms:** `program.bin` beginnt mit einem Kopf aus vier Wörtern: `"SCRP"`
  (`0x50524353`), Größe in Bytes, CRC-32 der Bytes nach dem Kopf, Einsprungadresse. Der Lader
  prüft ihn nach dem Speichertest (Test „program“, `SYS_SIM` Bit 4), leert den D-Cache, macht
  `fence.i` und springt mit `a0` = seine Ergebnisse (`SYS_SIM`-Bits), `a1`, `a2` = Position
  und angefangenes Wort im Protokoll. Das Programm setzt eigenes `gp`, `tp` und `mtvec`, sein
  Stack liegt oben im Boot-RAM (ab `0x8000` abwärts bis `0x6000`; der Lader bleibt darunter).
  Nach dem vollen Speichertest (Select beim Start) bleiben die Ergebnisse stehen; das Programm
  startet erst beim nächsten Start des Cores.

## Speicherkarte der CPU

| Adresse | Größe | Inhalt | Cache |
|---|---|---|---|
| `0x0000_0000` | 32 KB | Boot-RAM: der Lader, darüber der Stack des Programms. Reset-Vektor `0x0000_0000`, `mtvec` nach Reset `0x0000_0020` | ja |
| `0x1000_0000` | 64 MB | SDRAM: die Strecke ab `0x1210_0000` (Slot 3), das Programm ab `0x1300_0000` (Slot 2) | ja |
| `0xF000_0000` | 4 KB | System- und Speicher-Register | nein |
| `0xF000_1000` | 4 KB | Video-Register und Palette | nein |
| `0xF000_2000` | 4 KB | Eingabe | nein |
| `0xF000_8000` | 4 KB | Protokoll (`debug.txt`, siehe unten) | nein |
| `0xF100_0000` | 64 000 B | Framebuffer 0 | nein |
| `0xF101_0000` | 64 000 B | Framebuffer 1 | nein |
| `0xF400_0000` | 64 MB | SDRAM, dasselbe ungecacht | nein |

`0xF000_0000`–`0xFFFF_FFFF` ist für die CPU ungecacht (`StaticMemoryTranslatorPlugin`).
Lesen außerhalb der Bereiche liefert einen Busfehler (Trap), Schreiben dorthin wird
ignoriert. Code läuft nur aus dem Boot-RAM und aus `0x1000_0000`; ein Befehlsabruf von
anderswo, auch aus dem ungecachten SDRAM-Fenster, SRAM oder PSRAM, ist ein Zugriffsfehler.
Die SDRAM-, SRAM- und PSRAM-Zugriffe beschreibt `docs/speicher.md`: Byte-, Halbwort- und
Wortzugriffe; Lesen liefert immer das ganze Wort, die CPU wählt die Bytes. Code, der mit
Stores ins SDRAM kommt, braucht danach `fence.i` und drei weitere Befehle (`icache_sync()` in
`firmware/hw.h`); Daten, die nicht die CPU geschrieben hat (Bridge), brauchen vorher ein
Leeren des D-Cache (`0x0000500F`, `dcache_invalidate()`).

## Register

Alle Register sind 32 Bit, Zugriff als Wort.

| Adresse | Name | | Bedeutung |
|---|---|---|---|
| `0xF000_0000` | `SYS_ID` | R | `0x53435231` („SCR1“) |
| `0xF000_0004` | `SYS_SIM` | W | nur für die Simulation: die Testbench liest den Wert. Bit 31 = fertig. Bit 30 = Trap, dann Bit 29..0 = `mcause`; sonst Bit 29..0 = Ergebnis. Auf dem Pocket wirkungslos |
| `0xF000_0008` | `SYS_FLAGS` | R | Bit 0: läuft in der Simulation (nur das Simulations-Top setzt es; der Speichertest prüft dann kleinere Fenster) |
| `0xF000_000C` | `MEM_STATUS` | R | Bit 0 SDRAM-PLL eingerastet, Bit 1 SDRAM bereit, Bit 2 Bridge-Puffer übergelaufen, Bit 3 Bridge-Wort verworfen (SDRAM nicht bereit), Bit 4 alle angenommenen Schreibzugriffe erledigt. Bit 2 und 3 löscht Reset Enter |
| `0xF000_0010` | `MEM_BRIDGE_WORDS` | R | Bridge-Wörter ins SDRAM seit dem Einschalten oder seit Reset Enter |
| `0xF000_0014` | `MEM_REFRESH` | R | Zahl der AUTO REFRESH an das SDRAM seit dem Einschalten |
| `0xF000_0018` | `MEM_PHASE` | R/W | Schreiben: Bit 0 = `dram_clk` um einen Schritt (168 ps) verschieben, Bit 1 = später (sonst früher), Bit 8 = SDRAM neu initialisieren. Lesen: Bit 0 Verschiebung läuft, Bit 1 SDRAM bereit, Bit 31..16 Schritte seit dem Start (vorzeichenbehaftet; 80 Schritte = eine Periode) |
| `0xF000_0020` | `DS_ID` | W | Data-Slot-Lesen beim Pocket anfordern: Slot-Nummer |
| `0xF000_0024` | `DS_OFFSET` | W | Byte-Versatz in der Datei |
| `0xF000_0028` | `DS_ADDR` | W | Bridge-Adresse, an die geladen wird (im SDRAM gleich der CPU-Adresse) |
| `0xF000_002C` | `DS_LENGTH` | W | Bytes, ein Vielfaches von 4 |
| `0xF000_0030` | `DS_CTRL` | R/W | Schreiben: Bit 0 = anfordern. Lesen: Bit 0 läuft noch, Bit 6..4 Ergebnis des Pocket (0 = gut, 2 z. B. keine Datei im Slot) |
| `0xF000_1000` | `VID_STATUS` | R | Bit 0: vorderer Puffer (0/1). Bit 1: Tausch angefordert, noch nicht vollzogen. Bit 31..16: Bildzähler, zählt jedes Videobild (50 Hz) |
| `0xF000_1004` | `VID_SWAP` | W | beliebiger Wert: Tausch anfordern. Er geschieht in der nächsten Austastlücke; solange er aussteht (Bit 1), werden weitere Anforderungen ignoriert |
| `0xF000_1400` + 4·i | `VID_PAL[i]` | W | Palette des **hinteren** Puffers, Eintrag i (0..255): `0x00RRGGBB`. Nur solange kein Tausch aussteht (`VID_STATUS` Bit 1 = 0); sonst trifft es den Puffer, der gerade angezeigt wird |
| `0xF000_2000` | `IN_KEY` | R | Tasten (`cont1_key`): Bit 0 hoch, 1 runter, 2 links, 3 rechts, 4 A, 5 B, 6 X, 7 Y, 8 L, 9 R, 14 Select, 15 Start |
| `0xF000_2004` | `IN_JOY` | R | Analogsticks (`cont1_joy`) |
| `0xF000_2008` | `IN_TRIG` | R | Analog-Trigger (`cont1_trig`) |
| `0xF000_3000`–`3008` | `RS_V0`–`RS_V2` | W | Rasterizer (`raster.v`, dieselben Pixel wie `firmware/raster.c`): Eckpunkt, Bit 15..0 x, Bit 31..16 y, vorzeichenbehaftet in 1/16 Pixel; darf geschrieben werden, während ein Dreieck gezeichnet wird |
| `0xF000_300C`, `3010` | `RS_CLIP0`, `RS_CLIP1` | W | Clip-Rechteck in Pixeln: x0/y0, x1/y1 (ausschließlich), Bit 15..0 x, Bit 31..16 y |
| `0xF000_3014` | `RS_DRAW` | R/W | Schreiben: Bit 7..0 Farbe, Bit 8 Puffer, startet das Dreieck; der Schreibzugriff wartet, solange der Rasterizer zeichnet, Framebuffer-Zugriffe der CPU ebenso. Lesen: Bit 0 zeichnet noch |
| `0xF000_3018` | `RS_PIXELS` | R/W | gezeichnete Pixel; Schreiben löscht |
| `0xF000_301C`–`304C` | `RS_TEX` … `RS_CTRL` | W | Texturen (`firmware/hw.h`): Byte-Adresse von Texel (0, 0) im SDRAM, Größe und Durchsichtigkeit, Bezugspixel, Q, U, V mit Änderung je Pixel; `RS_CTRL` leert den Textur-Cache; `RS_DRAW` Bit 9 zeichnet texturiert |
| `0xF000_3050` | `RS_MISSES` | R/W | Textur-Zeilen aus dem SDRAM nachgeladen (Cache-Fehltreffer); Schreiben löscht |
| `0xF000_3054` | `RS_CLOCKS` | R/W | Takte, in denen der Rasterizer beschäftigt war; Schreiben löscht (auch die fünf folgenden) |
| `0xF000_3058`–`3068` | `RS_MISS_CLOCKS` … `RS_STARVED` | R | Takte, in denen der vorderste Pixel der Textur-Warteschlange auf seine Zeile aus dem SDRAM wartete; in der Kantenvorbereitung; im Aufwand je Zeile (Zeilen, Spanne, Werte am ersten Pixel); untätig, während das nächste Dreieck der Setup-Einheit auf das Textur-Fließband wartete; untätig, während Setup- oder Geometrie-Einheit arbeiteten |
| `0xF000_3080`, `3084` | `SU_CENTRE`, `SU_SCALE` | W | Setup-Einheit (`setup.v` mit `clipper.v`, bitgenau `firmware/setup.c`): Mitte der Ansicht in 1/16 Pixel (wie `RS_XY`), Brennweite in Pixeln mal 16 |
| `0xF000_3088`–`3090` | `SU_MODE`, `SU_TEX`, `SU_TEXSIZE` | W | wie `RS_DRAW` Bit 9..0, `RS_TEX`, `RS_TEXSIZE`, dazu in `SU_MODE` Bit 12..10 die Zahl der Ecken (3 oder 4); gelten ab der nächsten ersten Ecke für das ganze Vieleck |
| `0xF000_3094`–`30A0` | `SU_R`, `SU_U`, `SU_F`, `SU_S` | W | eine Ecke in Kamerasicht als Float: rechts, oben, vorn (nach der nahen Ebene, > 0), Texel-Spalte |
| `0xF000_30A4`, `30A8` | `SU_T_NEXT`, `SU_T_FIRST` | W | Texel-Zeile (Float) und Übergabe der Ecke: setzt das Vieleck fort bzw. beginnt ein neues. Mit der letzten Ecke geht das Vieleck an den Clipper: geschnitten an der nahen Ebene (Tiefe 128), am Schutzband (\|r\|, \|u\| ≤ 14 × Tiefe) und an der fernen Ebene, dann ein Fächer von Dreiecken aus der ersten Ecke. Der Schreibzugriff wartet, solange der Eingang noch das vorige Vieleck hält; `RS_DRAW` und Framebuffer-Zugriffe warten, bis Setup-Einheit und Rasterizer fertig sind, `RS_DRAW` Bit 0 zeigt beide |
| `0xF000_30AC` | `SU_CLOCKS` | R/W | Takte, in denen die Setup-Einheit gerechnet hat; Schreiben löscht |
| `0xF000_30B0` | `SU_FAR` | W | Tiefe der fernen Ebene mit 8 Nachkommabits (ganze Zahl) |
| `0xF000_3144`, `3148` | `GU_MLB`, `GU_MODE` | W | SDRAM-Byte-Adresse der Materialdatei (MLB); Bit 8 der Puffer, in den gezeichnet wird |
| `0xF000_314C` | `GU_GO` | W | Bit 2..0 die Flächenliste für die Seite des Auges, startet das Bauteil. Wartet, solange die Einheit am vorigen arbeitet; die Register davor dürfen schon für das nächste geschrieben werden. Die Einheit liest das Mesh selbst (4-KB-Lese-Cache, Zeilen über den Port des Rasterizers), rechnet die Eckpunkte um, lässt Rückseiten und Material 224 weg und gibt jede Fläche an die Setup-Einheit; Ecken der CPU an die Setup-Einheit warten, bis sie fertig ist |
| `0xF000_3150`, `3154` | `GU_FACES`, `GU_CLOCKS` | R/W | weitergegebene Flächen, Takte beschäftigt; Schreiben löscht |
| `0xF000_3158` | `GU_INVAL` | W | den Lese-Cache leeren (nachdem sich Mesh-Daten geändert haben) |
| `0xF000_315C` | `GU_PUSH` | W | Geometrie-Einheit (`geom.v`, bitgenau `firmware/geom.c`): das nächste Wort eines Bauteils in ihren Puffer (2048 Wörter, 113 Bauteile); wartet nur, wenn er voll ist. Ein Bauteil sind 18 Wörter: lokal nach Kamerasicht (Zeilen r, u, f der Spalten x, y, z, 16 Nachkommabits, innerhalb ±2^17), Ursprung in Kamerasicht und Auge im Rahmen des Bauteils (je 8 Nachkommabits), SDRAM-Byte-Adressen von Mesh-Objekt und Block, Bit 2..0 die Flächenliste für die Seite des Auges, Bit 3 ein Block eines beweglichen Teils (seine Bilder spiegeln ihn, die Rückseiten sind die anderen, wie im Spiel bei `0x8acf0`) und Bit 29..16 die Texturphase (von n Bildern einer Textur, Texturdatensatz +0, wird Bild n × Phase >> 14 gezeichnet, wie im Spiel: Streckentexturen nach der Animationsuhr, beim Auto Lackierung, Bremslicht und Radbild). Die Einheit nimmt ein Bauteil, sobald es ganz im Puffer liegt, liest das Mesh selbst (4-KB-Lese-Cache, Zeilen über den Port des Rasterizers), rechnet die Eckpunkte um, lässt Rückseiten und Material 224 weg und gibt jede Fläche an die Setup-Einheit; Ecken der CPU an die Setup-Einheit warten, bis sie fertig ist |
| `0xF000_3180` | `BL_PUSH` | W | Blitter (`blit.v`, bitgenau `firmware/blit.c`): das nächste Wort eines Befehls in seinen Puffer (512 Wörter, 128 Befehle); wartet nur, wenn er voll ist. Ein Befehl sind 4 Wörter: Wort 0 die SDRAM-Byte-Adresse des ersten Quellpixels (Linie: Bit 9..0 x, 25..16 y ihres Endes), Wort 1 Bit 9..0 x, 25..16 y des ersten Pixels bzw. Linienanfangs (mit Vorzeichen), 29..28 die Art (0 Kopie, 1 Schrift, 2 Linie, 3 Füllen), 30 der Puffer, 31 bei Kopien deckend; Wort 2 Breite und Höhe (Linie: Clip links, oben), Wort 3 Bit 9..0 Bytes von Zeile zu Zeile (Linie: Bit 8..0 und 24..16 Clip rechts, unten, ausschließlich), 31..24 die Farbe. Kopie: das Byte, 0 durchsichtig (deckend: auch 0); Schrift: Schattierung 1..15 als Farbe × 16 + Schattierung; Linie: Bresenham, die Pixel im Clip; Füllen: das Rechteck in der Farbe. Kopien, Schrift und Füllen werden auf den Bildschirm beschnitten. Der Blitter beginnt einen Befehl nur, solange Geometrie-Einheit, Setup-Einheit und Rasterizer stillstehen: den Hintergrund vor der Szene (die Firmware hält das erste Bauteil zurück, bis er fertig ist), die Rennanzeige danach. Kopie und Schrift ein Pixel pro Takt, die Quelle zeilenweise über den gemeinsamen Zeilen-Port; Füllen ein Wort (vier Pixel) pro Takt |
| `0xF000_3184` | `BL_CLOCKS` | R/W | Takte, in denen der Blitter gearbeitet hat; Schreiben löscht |

## Framebuffer

Je 320 × 200 Bytes, ein Byte je Pixel (Palettenindex), Zeile für Zeile, das Pixel (x, y)
an Offset `y*320 + x`. Byte-, Halbwort- und Wortzugriffe; Lesen geht auch. Byte-Reihenfolge
wie im Speicher der CPU: Das Pixel mit dem kleinsten x liegt an der kleinsten Adresse.

Ablauf eines Bildes: warten, bis `VID_STATUS` Bit 1 = 0 ist. Hinterer Puffer = nicht Bit 0.
Den hinteren Puffer und dessen Palette beschreiben, dann `VID_SWAP` schreiben.

## Protokoll auf die SD-Karte

Ohne JTAG ist das der Weg, Fehler und Messwerte vom Pocket zu holen: Die Firmware schreibt
Text in den 4-KB-Bereich ab `0xF000_8000` (Byte-, Halbwort- und Wortzugriffe, Lesen geht
auch; little-endian wie sonst auch, Zeilen mit `\n`, der Rest bleibt `\0`). Der Bereich ist
der nichtflüchtige Data-Slot `debug.txt` (id 1, Bridge-Adresse `0x0010_0000`): Beim Beenden
des Cores im Menü („Quit“) und beim Ausschalten liest der Pocket ihn über die Bridge aus und
speichert ihn als `/Saves/screamer/Chainy.Screamer/debug.txt` (Ruhezustand unterstützt der
Core nicht, `core.json`: `sleep_supported: false`). Das klappt auch nach einem Absturz der
CPU, weil das Auslesen reine Hardware ist. `soc.v` dreht dabei die Byte-Reihenfolge, damit
die Datei lesbarer Text ist, und trägt nach dem Start die Größe 4096 für den Slot in die
Slot-Tabelle des Cores ein (die sonst nur beim Laden einer vorhandenen Datei gesetzt wird).

Beim Laden schreibt der Pocket die alte `debug.txt` (oder `0xFF`, wenn es sie nicht gibt) in
den Bereich; `soc.v` ignoriert das. Nach dem Laden des Cores steht dort also `0`, bis die
Firmware schreibt; läuft die CPU gar nicht los, besteht die gespeicherte Datei aus Nullen
statt aus dem alten Protokoll. Ein Neuladen der Firmware aus dem Menü ist nur ein Reset: Bis
die neue Firmware den Bereich löscht, steht dort noch das Protokoll des vorigen Laufs.

Die Firmware löscht den Bereich beim Start, schreibt eine Kopfzeile, die Ergebnisse der
Selbsttests und Messwerte; der Trap-Handler schreibt Ursache und Adresse eines Absturzes
dazu (`mcause`, `mepc`, `mtval`).

## Eigenheiten der CPU

- `reset` des VexRiscv ist aktiv high und asynchron; `soc.v` gibt ihn synchron zu `clk` frei.
- Es gibt kein `mscratch`; nicht vorhandene CSRs lösen keinen Trap aus, `ebreak` einen
  Trap wegen illegalen Befehls.
- Der Befehls-Cache (`InstructionCache.scala`) löscht seine Fehlermarke `hadError` zwar,
  wenn eine Zeile fertig geladen ist, setzt sie aber im selben Takt wieder, wenn das letzte
  Wort der Zeile einen Fehler meldet; dann gilt auch die nächste geladene Zeile als fehlerhaft,
  und der Trap-Handler läuft in einen zweiten Trap. `soc.v` meldet den Fehler einer Zeile
  außerhalb des Boot-RAMs deshalb nur mit den ersten sieben Wörtern; die Zeile ist damit
  trotzdem als fehlerhaft markiert. Der Daten-Cache hat das Problem nicht.
- Das fused multiply-add der FPU rundet das Produkt vor der Addition (`FpuCore.scala`:
  „FMA Precision lost“). GCC macht aus `a*b+c` standardmäßig `fmadd.s`; wo Ergebnisse bitgleich
  zu einem PC-Build sein müssen (Physik von OpenScreamer), mit `-ffp-contract=off` übersetzen.

## Eigene Befehle (CFU)

`custom-0` (Opcode `0x0B`), R-Format, `function_id = {funct7, funct3}`:

| funct7 | funct3 | Ergebnis |
|---|---|---|
| 0 | 0 | `(rs1 * rs2) >> 16`, vorzeichenbehaftet (16.16-Festkomma) |

```c
static inline int32_t fixmul(int32_t a, int32_t b)
{
    int32_t r;
    __asm__ volatile (".insn r 0x0B, 0, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
```

## Prüfung in der Simulation (`sim/soc`)

Die Testbench lädt die Slots wie der Pocket (höchstens ein Bridge-Wort alle 88 Takte, vor dem
SDRAM-Slot Warten auf die Quittung), lässt die CPU laufen und prüft:

- `SYS_SIM`: Die Firmware schreibt, sobald sie drei Bilder gezeichnet und getauscht hat,
  `0x80000000 | bits`. Bit 0 = Ganzzahl, Bit 1 = Fließkomma (add, mul, div, sqrt, Umwandlung),
  Bit 2 = CFU; Bit 3..12 = Speichertest (`firmware/memtest.c`): SDRAM bereit, Slot, Phase,
  Datenbus, Byte-Lanes, Adressbus, Muster, Code, Refresh, Schreib-Status. Erwartet:
  `0x80001FFF`. Bei einem Trap
  schreibt sie `0xC0000000 | mcause`.
- **Modell:** Das SDRAM-Modell meldet keinen Fehler (Befehlsfolgen, Zeiten, Buskonflikte),
  der längste Abstand zwischen zwei AUTO REFRESH bleibt unter 580 Takten.
- **Prüfstreifen:** Die oberen 8 Zeilen jedes von der Firmware gezeichneten Bildes haben in
  Spalte x < 256 den Palettenindex x, und die Palette jedes Bildes ist
  `pal[i] = (i << 16) | ((255 - i) << 8) | ((i * 3) & 255)`. Die Testbench vergleicht diese
  Pixel im angezeigten Bild.
- **Protokoll:** Die Testbench liest es über die Bridge so aus wie der Pocket (ein Wort mehr,
  die erste Antwort verworfen), erwartet die Kopfzeile, kein „FAIL", die Fenster der
  Simulation und im ersten Lauf „SDRAM slot: ok". Vor dem Start prüft sie, dass `soc.v` das
  Laden des Protokoll-Slots ignoriert.
- **Resets** wie beim Neuladen der Firmware aus dem Menü (nur `firmware.bin` neu): einmal,
  während Puffer 1 vorn ist (im Reset müssen Tausch-Anforderung, vorderer Puffer und `SYS_SIM`
  0 sein), einmal mitten im SDRAM-Verkehr des Speichertests. Der letzte Lauf muss wie der
  erste bestehen, den SDRAM-Slot als „skipped" melden und darf vor der ersten Anforderung der
  CPU nichts tauschen.

Der SDRAM-Controller hat dazu einen eigenen Zufallstest (`sim/sdram`, siehe
`docs/speicher.md`). Gegenproben: `tools/sim_faults.sh` baut in je einem Lauf einen Fehler in
das SDRAM-Modell ein; mit jedem anderen erwarteten Wert als `0x80001FFF` (und ohne
Trap-Bits) verlangt die Testbench genau diesen Wert, mindestens ein „FAIL" im Protokoll und
lässt die Resets weg.

Fehlertests: `tools/fw.sh FAULT=IFETCH` baut eine Firmware, die nach den Selbsttests an
`0x0000_8000` springt, hinter das Boot-RAM (nach `build/firmware-IFETCH/`, nicht auf die
SD-Karte). Erwartet: `SYS_SIM = 0xC0000001` (Zugriffsfehler beim Befehlsabruf), der Kasten des
Trap-Handlers im Bild und die Zeile `TRAP mcause=00000001` im Protokoll. Das prüft auch, dass
der Trap-Handler nach dem Fehler aus dem Befehls-Cache läuft (siehe „Eigenheiten der CPU“):

```sh
tools/fw.sh FAULT=IFETCH
tools/sim.sh soc build/firmware-IFETCH/firmware.bin build/sim/soc_ifetch 80 c0000001
```

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
  Pocket lädt alle Data-Slots vor „Reset Exit“, die Firmware liegt beim Start also schon im
  Boot-RAM. Beim Neuladen der Firmware aus dem Menü setzt der Pocket den Core zurück
  (Parameter-Bit 6), lädt aber keinen neuen Bitstream. Nach jedem Reset ist Puffer 0 vorn,
  kein Tausch steht aus und `SYS_SIM` ist 0. Bis zum ersten Tausch zeigt `video_out` Puffer 0
  mit Palette 0: nach dem Laden des Cores schwarz, nach einem Neuladen der Firmware, was der
  vorige Lauf dort hinterlassen hat.
- **Firmware:** zwei Teile. Data-Slot 0 `firmware.bin` (höchstens 32 KB) an Bridge-Adresse
  `0x00000000` ins Boot-RAM: der Lader (Selbsttests, Speichertest). Data-Slot 2
  `program.bin` an `0x1300_0000` ins SDRAM: das Programm. Die Bridge liefert 32-Bit-Wörter
  big-endian (das erste Byte der Datei in Bit 31:24), die CPU ist little-endian: `soc.v`
  dreht die Byte-Reihenfolge beim Schreiben um, beide sind ganz normale little-endian-Abbilder.
- **Start des Programms:** `program.bin` beginnt mit einem Kopf aus vier Wörtern: `"SCRP"`
  (`0x50524353`), Größe in Bytes, CRC-32 der Bytes nach dem Kopf, Einsprungadresse. Der Lader
  prüft ihn nach dem Speichertest (Test „program“, `SYS_SIM` Bit 4), leert den D-Cache, macht
  `fence.i` und springt mit `a0` = seine Ergebnisse (`SYS_SIM`-Bits), `a1`, `a2` = Position
  und angefangenes Wort im Protokoll. Das Programm setzt eigenes `gp`, `tp` und `mtvec`, sein
  Stack liegt oben im Boot-RAM (ab `0x8000` abwärts bis `0x6000`; der Lader bleibt darunter).
  Der kurze Speichertest schreibt im SDRAM nur unterhalb von 32 MB und ins letzte Fenster, der
  volle (Select beim Start) überschreibt das Programm: Dann bleiben nur die Ergebnisse stehen.

## Speicherkarte der CPU

| Adresse | Größe | Inhalt | Cache |
|---|---|---|---|
| `0x0000_0000` | 32 KB | Boot-RAM: der Lader, darüber der Stack des Programms. Reset-Vektor `0x0000_0000`, `mtvec` nach Reset `0x0000_0020` | ja |
| `0x1000_0000` | 64 MB | SDRAM: Texturen ab `0x1210_0000` (Slot 3), das Programm ab `0x1300_0000` (Slot 2) | ja |
| `0xF000_0000` | 4 KB | System- und Speicher-Register | nein |
| `0xF000_1000` | 4 KB | Video-Register und Palette | nein |
| `0xF000_2000` | 4 KB | Eingabe | nein |
| `0xF000_8000` | 4 KB | Protokoll (`debug.txt`, siehe unten) | nein |
| `0xF100_0000` | 64 000 B | Framebuffer 0 | nein |
| `0xF101_0000` | 64 000 B | Framebuffer 1 | nein |
| `0xF400_0000` | 64 MB | SDRAM, dasselbe ungecacht | nein |
| `0xF800_0000` | 256 KB | SRAM | nein |
| `0xFA00_0000` | 16 MB | PSRAM `cram0` (Die 0, Die 1 ab `0xFA80_0000`) | nein |

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
  Bit 2 = CFU; Bit 3..20 = Speichertest (`firmware/memtest.c`): SDRAM bereit, Slot, Phase,
  Datenbus, Byte-Lanes, Adressbus, Muster, Code, Refresh, Schreib-Status; SRAM Datenbus,
  Byte-Lanes, Adressbus, Muster; `cram0` dieselben vier. Erwartet: `0x801FFFFF`. Bei einem Trap
  schreibt sie `0xC0000000 | mcause`.
- **Modelle:** SDRAM-, SRAM- und PSRAM-Modell melden keinen Fehler (Befehlsfolgen, Zeiten,
  Buskonflikte, Software-Access-Wort), der längste Abstand zwischen zwei AUTO REFRESH bleibt
  unter 580 Takten.
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
ein Speichermodell ein; mit jedem anderen erwarteten Wert als `0x801FFFFF` (und ohne
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

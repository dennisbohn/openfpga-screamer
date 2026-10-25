# CPU-System (Schritt 1: die CPU zeichnet)

Vertrag zwischen Hardware (`core/src/fpga/core/soc.v`) und Firmware (`firmware/`). Wer
etwas daran ändert, ändert beide Seiten und dieses Dokument.

## Überblick

- **CPU:** VexRiscv `VexPocketIMFC` (RV32IM + F + CfuPlugin, 8 KB I-Cache, 8 KB D-Cache
  write-through, keine MMU), erzeugt mit `tools/vexriscv/gen.sh`.
- **Takt:** In Schritt 1 läuft alles auf `clk_74a` (74,25 MHz), ohne eigene PLL und ohne
  Taktübergang zur Bridge. Ab dem SDRAM-Schritt bekommt das System einen eigenen Takt.
- **Reset:** Die CPU steht im Reset, solange `reset_n` aus `core_bridge_cmd` low ist. Der
  Pocket lädt alle Data-Slots vor „Reset Exit“, die Firmware liegt beim Start also schon im
  Boot-RAM. Beim Neuladen der Firmware aus dem Menü setzt der Pocket den Core zurück
  (Parameter-Bit 6).
- **Firmware:** Data-Slot `firmware.bin` (höchstens 16 KB) an Bridge-Adresse `0x00000000`,
  geschrieben ins Boot-RAM. Die Bridge liefert 32-Bit-Wörter big-endian (das erste Byte der
  Datei in Bit 31:24), die CPU ist little-endian: `soc.v` dreht die Byte-Reihenfolge beim
  Schreiben um, `firmware.bin` ist ein ganz normales little-endian-Abbild.

## Speicherkarte der CPU

| Adresse | Größe | Inhalt | Cache |
|---|---|---|---|
| `0x0000_0000` | 16 KB | Boot-RAM: Code, Daten, Stack. Reset-Vektor `0x0000_0000`, `mtvec` nach Reset `0x0000_0020` | ja |
| `0xF000_0000` | 4 KB | System-Register | nein |
| `0xF000_1000` | 4 KB | Video-Register und Palette | nein |
| `0xF000_2000` | 4 KB | Eingabe | nein |
| `0xF000_8000` | 4 KB | Protokoll (`debug.txt`, siehe unten) | nein |
| `0xF100_0000` | 64 000 B | Framebuffer 0 | nein |
| `0xF101_0000` | 64 000 B | Framebuffer 1 | nein |

`0xF000_0000`–`0xFFFF_FFFF` ist für die CPU ungecacht (`StaticMemoryTranslatorPlugin`).
Lesen außerhalb der Bereiche liefert einen Busfehler (Trap), Schreiben dorthin wird
ignoriert.

## Register

Alle Register sind 32 Bit, Zugriff als Wort.

| Adresse | Name | | Bedeutung |
|---|---|---|---|
| `0xF000_0000` | `SYS_ID` | R | `0x53435231` („SCR1“) |
| `0xF000_0004` | `SYS_SIM` | W | nur für die Simulation: die Testbench liest den Wert. Bit 31 = fertig. Bit 30 = Trap, dann Bit 29..0 = `mcause`; sonst Bit 29..0 = Ergebnis. Auf dem Pocket wirkungslos |
| `0xF000_1000` | `VID_STATUS` | R | Bit 0: vorderer Puffer (0/1). Bit 1: Tausch angefordert, noch nicht vollzogen. Bit 31..16: Bildzähler, zählt jedes Videobild (60 Hz) |
| `0xF000_1004` | `VID_SWAP` | W | beliebiger Wert: Tausch anfordern. Er geschieht in der nächsten Austastlücke; solange er aussteht (Bit 1), werden weitere Anforderungen ignoriert |
| `0xF000_1400` + 4·i | `VID_PAL[i]` | W | Palette des **hinteren** Puffers, Eintrag i (0..255): `0x00RRGGBB`. Nur solange kein Tausch aussteht (`VID_STATUS` Bit 1 = 0); sonst trifft es den Puffer, der gerade angezeigt wird |
| `0xF000_2000` | `IN_KEY` | R | Tasten (`cont1_key`): Bit 0 hoch, 1 runter, 2 links, 3 rechts, 4 A, 5 B, 6 X, 7 Y, 8 L, 9 R, 14 Select, 15 Start |
| `0xF000_2004` | `IN_JOY` | R | Analogsticks (`cont1_joy`) |
| `0xF000_2008` | `IN_TRIG` | R | Analog-Trigger (`cont1_trig`) |

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
Core nicht, `core.json`: `sleep_supported: false`). Gibt es die Datei noch nicht, füllt der
Pocket den Bereich beim Laden mit `0xFF`; läuft die CPU gar nicht los, steht genau das in der
gespeicherten Datei. Das klappt auch nach einem Absturz der
CPU, weil das Auslesen reine Hardware ist. `soc.v` dreht dabei die Byte-Reihenfolge, damit
die Datei lesbarer Text ist, und trägt nach dem Start die Größe 4096 für den Slot in die
Slot-Tabelle des Cores ein (die sonst nur beim Laden einer vorhandenen Datei gesetzt wird).

Die Firmware löscht den Bereich beim Start, schreibt eine Kopfzeile, die Ergebnisse der
Selbsttests und Messwerte; der Trap-Handler schreibt Ursache und Adresse eines Absturzes
dazu (`mcause`, `mepc`, `mtval`).

## Eigenheiten der CPU

- `reset` des VexRiscv ist aktiv high und asynchron; `soc.v` gibt ihn synchron zu `clk` frei.
- Es gibt kein `mscratch`; nicht vorhandene CSRs lösen keinen Trap aus, `ebreak` einen
  Trap wegen illegalen Befehls.
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

Die Testbench lädt `firmware.bin` über die Bridge, lässt die CPU laufen und prüft:

- `SYS_SIM`: Die Firmware schreibt, sobald sie drei Bilder gezeichnet und getauscht hat,
  `0x80000000 | bits` mit den Ergebnissen ihrer Selbsttests, Bit 0 = Ganzzahl, Bit 1 =
  Fließkomma (add, mul, div, sqrt, Umwandlung), Bit 2 = CFU. Erwartet: `0x80000007`. Bei einem
  Trap schreibt sie `0xC0000000 | mcause`.
- **Prüfstreifen:** Die oberen 8 Zeilen jedes von der Firmware gezeichneten Bildes haben in
  Spalte x < 256 den Palettenindex x, und die Palette jedes Bildes ist
  `pal[i] = (i << 16) | ((255 - i) << 8) | ((i * 3) & 255)`. Die Testbench vergleicht diese
  Pixel im angezeigten Bild.

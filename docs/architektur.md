# Architektur

Stand: 2026-10-25. Gebaut und auf dem Pocket getestet: Video-Ausgabe 320×200 aus einem
8-Bit-Framebuffer im PSRAM (`cram1`) über Zeilenpuffer und Palette, geladen über einen
Data-Slot (586 ALMs, 6 M10K). Alles Weitere sind Schätzungen, bis die Synthese echte
Zahlen liefert.

## Hardware des Pocket (Haupt-FPGA)

Cyclone V 5CEBA4F23C8: 18.480 ALMs, 308 M10K (je 10 Kbit), 66 DSP-Blöcke, 4 PLLs.
Getrennte Speicherbusse: SDRAM 64 MB, PSRAM `cram0` und `cram1` je 16 MB, SRAM 256 KB
(alle 16 Bit breit).

## Aufteilung

![Geplante Architektur: Datenfluss vom RISC-V über Geometrie-Einheit und Rasterizer in den Framebuffer, Speicher und Ein-/Ausgänge des Pocket](architektur.svg)

Durchgezogen: gebaut und auf dem Pocket getestet. Gestrichelt: geplant. Der RISC-V schickt
Objekte mit ihren Matrizen an die Geometrie-Einheit, die liefert Dreiecke an den
Rasterizer; der liest Texel aus `cram0` und schreibt 8-Bit-Pixel in den Framebuffer in
`cram1`, aus dem `video_out` jede Zeile vor ihrer Ausgabe holt.

| Speicher | Inhalt |
|---|---|
| SDRAM | Programm, Geometrie, Spielzustand, Sounds |
| cram0 | Texturen der aktuellen Strecke (`MAPn.MLB`, ≤ 3 MB) |
| cram1 | Framebuffer, 8 Bit, doppelt gepuffert |
| Block-RAM | CPU-Caches, Textur-Cache, Palette, Schattierungstabellen, ggf. Tiefenpuffer |

## Speicherbedarf eines Rennens (gemessen, entpackt)

| Daten | Datei | Größe |
|---|---|---|
| Texturen | `GFX/MAPnL/H.DAT` → `MAPn.MLB` | 2,3–2,9 MB |
| Streckengeometrie | `MAPn.UNR` | 0,6–1,6 MB |
| Autos, Requisiten | `*.3DX` | ca. 270 KB |
| Kartenraster | `MAPn.MMP` | 100–240 KB |
| Himmel | `MAPn.BCK` | ca. 70 KB |
| Fahrlinien, KI, Kameras | `DATA/MAPn.DAT` | 17–41 KB |
| HUD, Cockpit | `GFX/GAME.DAT` | 2,3 MB (L- und H-Satz) |
| Effekte (8-Bit-PCM) | `DATA/SOUND.DAT` | 1,9 MB |
| Tabellen | `TABS/*.TAB` | ca. 410 KB |

## Spieldaten auf der SD-Karte

Die originalen Dateien, unverändert und flach in `Assets/screamer/common/` (die Namen sind
über `DATA`, `GFX` und `TABS` hinweg eindeutig). Je Datei ein Data-Slot mit `deferload`:
der Core liest das Inhaltsverzeichnis am Dateiende und holt sich die Einträge, die er
braucht. 27 Dateien (Grenze: 32 Slots):

- `DATA/`: alle 16 (`GEN`, `LOCAL`, `MUSIC`, `SOUND`, `MAP1`–`MAP6`, `MAP1I`–`MAP6I`)
- `GFX/`: `GAME`, `GRAF`, `MENU`, `MAP1L`–`MAP6L` (die H-Dateien sind für 640×400, nicht nötig)
- `TABS/`: `PERSP01.TAB`, `VIS.TAB`

## Geschätzte Belegung

![Platzbudget im FPGA: Logik geplant 53–79 %, Block-RAM 23–31 % (55–63 % mit Tiefenpuffer), DSP 35–56 %](ressourcen.svg)

Die Grafik erzeugt `tools/make_budget_svg.py` aus denselben Zahlen wie diese Tabelle; wenn
die Synthese eine Schätzung ersetzt, beides anpassen.

| Komponente | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue-Rahmen (**gemessen**, unverändertes Template) | 414 | 2 | 0 |
| Framebuffer + Video (**gemessen**: `fb_psram`, `video_out`, PSRAM-Controller) | 172 | 4 | 0 |
| VexRiscv RV32IM + 2× 8 KB Cache | 1.200–1.600 | 18–22 | 4 |
| FPU (RV32F) | 1.500–2.200 | 0–2 | 2–4 |
| Speicher-Controller (SDRAM, `cram0`) + Arbiter | 900–1.400 | 2–4 | 0 |
| Geometrie-Einheit | 2.500–4.000 | 4–8 | 12–18 |
| Rasterizer (Logik) | 2.000–3.000 | 3–6 | 4–8 |
| Textur-Cache 16 KB | 100–200 | 16 | – |
| Schattierungs-/Transparenztabellen | – | 16–24 | – |
| JTOPL | 800–1.200 | 3–5 | 0–1 |
| PCM-Mixer | 300–500 | 2 | 1–2 |
| **Summe** | **9.886–14.686 (53–79 %)** | **70–95 (23–31 %)** | **23–37 (35–56 %)** |
| Tiefenpuffer 320×200×16 (nur Variante B) | – | +100 (dann 55–63 %) | – |

Knapp wird die Logik: ab etwa 80 % Belegung wird es auf dem Cyclone V schwer, die
Taktfrequenz zu halten. Block-RAM und DSP haben Reserve, auch mit Tiefenpuffer.

## Entschieden

- **Sprache:** eigener Code in Verilog (wie das Template). Der VexRiscv wird einmalig aus
  einer Scala-Konfiguration als Verilog erzeugt, eigene CPU-Befehle laufen über sein
  CFU-Plugin. Gleiche Leistung wie SpinalHDL, leichter zu lesen.
- **Spieldaten:** Originaldateien flach in `Assets/screamer/common/` (siehe oben).

## Offene Entscheidungen

- **Sichtbarkeit:** Das Original malt ganze Objekte von hinten nach vorne (Maler-Algorithmus,
  Abstand des Objekt-Mittelpunkts), die Streckendaten verlassen sich darauf (Grasflächen
  unter der Straße). Variante A: so wie das Original, kein Tiefenpuffer. Variante B:
  Tiefenpuffer plus Sonderregeln wie in OpenScreamer. Entscheidung am C-Modell.
- **Perspektivkorrektur:** pro Pixel oder alle 8/16 Pixel. Entscheidung am C-Modell.
- **Zahlenformate:** Rasterizer und Geometrie in Festkomma (Bildschirmkoordinaten mit
  mindestens 4 Nachkomma-Bits), die Physik auf der CPU in `float` wie im Original.

## Vorgehen

1. Grundgerüst: Template bauen, dann Video aus 8-Bit-Framebuffer mit Palette, Eingabe, JTOPL.
2. VexRiscv mit FPU, Spiellogik von OpenScreamer bare-metal, erst mit Software-Renderer.
3. Bitgenaue C-Modelle von Rasterizer und Geometrie-Einheit, dann die Hardware dazu,
   jeweils in Verilator gegen das Modell geprüft.

# Architektur

Stand: 2026-10-25. Gebaut und auf dem Pocket getestet: Video-Ausgabe 320×200 aus zwei
8-Bit-Framebuffern im Block-RAM, je mit eigener Palette, getauscht zu Beginn eines Bildes;
bis die CPU zeichnet, füllen zwei Data-Slots die Puffer und Taste A tauscht sie (482 ALMs,
132 M10K). Davor lief der Framebuffer im PSRAM `cram1` (Commit `b148783`). Alles Weitere
sind Schätzungen, bis die Synthese echte Zahlen liefert.

## Hardware des Pocket (Haupt-FPGA)

Cyclone V 5CEBA4F23C8: 18.480 ALMs, 308 M10K (je 10 Kbit), 66 DSP-Blöcke, 4 PLLs.
Getrennte Speicherbusse: SDRAM 64 MB, PSRAM `cram0` und `cram1` je 16 MB, SRAM 256 KB
(alle 16 Bit breit).

## Aufteilung

![Geplante Architektur: Datenfluss vom RISC-V über Geometrie-Einheit und Rasterizer in den Framebuffer, Speicher und Ein-/Ausgänge des Pocket](architektur.svg)

Grün: gebaut und auf dem Pocket getestet. Grau gestrichelt: geplant. Drei Bilder sind
gleichzeitig in Arbeit: Die CPU berechnet Bild N+1 und schreibt dessen nach Abstand
sortierte Objektliste ins SRAM, die Geometrie-Einheit liest die Liste von Bild N und
liefert Dreiecke an den Rasterizer, der Texel aus `cram0` liest und Bild N in den
hinteren Framebuffer zeichnet, während `video_out` den vorderen zeigt (Bild N−1).
Getauscht wird zu Beginn eines Bildes; Befehlslisten und Framebuffer sind doppelt
gepuffert.

| Speicher | Inhalt |
|---|---|
| SDRAM | Programm, Geometrie, Spielzustand, Sounds |
| SRAM | Befehlslisten der Geometrie-Einheit, doppelt gepuffert (je ~20–30 KB) |
| cram0 | Texturen der aktuellen Strecke (`MAPn.MLB`, ≤ 3 MB), später im Burst-Modus |
| cram1 | frei |
| Block-RAM | zwei Framebuffer (2 × 64 KB) und zwei Paletten, CPU-Caches, Textur-Cache, Schattierungstabellen |

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

![Platzbudget im FPGA: Logik geplant 53–79 %, Block-RAM 23–31 %, DSP 35–56 %](ressourcen.svg)

Die Grafik erzeugt `tools/make_budget_svg.py` aus denselben Zahlen wie diese Tabelle; wenn
die Synthese eine Schätzung ersetzt, beides anpassen.

| Komponente | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue-Rahmen (**gemessen**, unverändertes Template) | 414 | 2 | 0 |
| Framebuffer ×2 + Video (**gemessen**: `framebuffer`, `video_out`, `fb_bridge`) | 68 | 130 | 0 |
| VexRiscv RV32IM + 2× 8 KB Cache | 1.200–1.600 | 18–22 | 4 |
| FPU (RV32F) | 1.500–2.200 | 0–2 | 2–4 |
| Speicher-Controller (SDRAM, `cram0`) + Arbiter | 900–1.400 | 2–4 | 0 |
| SRAM-Controller (Befehlslisten) | 100–200 | – | 0 |
| Geometrie-Einheit | 2.500–4.000 | 4–8 | 12–18 |
| Rasterizer (Logik) | 2.000–3.000 | 3–6 | 4–8 |
| Textur-Cache 16 KB | 100–200 | 16 | – |
| Schattierungs-/Transparenztabellen | – | 16–24 | – |
| JTOPL | 800–1.200 | 3–5 | 0–1 |
| PCM-Mixer | 300–500 | 2 | 1–2 |
| **Summe** | **9.882–14.782 (53–80 %)** | **196–221 (64–72 %)** | **23–37 (35–56 %)** |

Knapp wird die Logik: ab etwa 80 % Belegung wird es auf dem Cyclone V schwer, die
Taktfrequenz zu halten. Block-RAM trägt jetzt die beiden Framebuffer (128 M10K) und hat
danach noch Reserve, DSP ebenso. Einen Tiefenpuffer gibt es nicht, siehe „Entschieden“.

## Entschieden

- **Sprache:** eigener Code in Verilog (wie das Template). Der VexRiscv wird einmalig aus
  einer Scala-Konfiguration als Verilog erzeugt, eigene CPU-Befehle laufen über sein
  CFU-Plugin. Gleiche Leistung wie SpinalHDL, leichter zu lesen.
- **Spieldaten:** Originaldateien flach in `Assets/screamer/common/` (siehe oben).
- **Sichtbarkeit wie im Original, ohne Tiefenpuffer** (Maler-Algorithmus): Die CPU
  sortiert ganze Objekte nach dem Abstand ihres Mittelpunkts von der Kamera und gibt sie
  von hinten nach vorn aus (im Original `0x83164`: `dist² >> 2`). Innerhalb eines Objekts
  liegt die Reihenfolge in den Daten: Jeder Mesh-Block hat acht vorberechnete
  Flächenlisten, eine je Oktant der Blickrichtung, ohne die von dort unsichtbaren Flächen
  (Header +0x28..+0x44, OpenScreamer `docs/NOTES.md`). Die Geometrie-Einheit wählt die
  Liste über die drei Vorzeichen der Blickrichtung im Objektraum; sie sortiert nicht und
  braucht keinen Rückseitentest. Die Streckendaten verlassen sich auf diese Reihenfolge
  (Grasflächen unter der Straße, zwei Objekte an einer Stelle), OpenScreamer brauchte
  dafür mit seinem Tiefenpuffer Sonderregeln. Nötig bleibt das Clipping an der vorderen
  Ebene. Es spart 100 M10K; dafür wird jedes verdeckte Pixel erneut geschrieben, was die
  Bandbreite zum Framebuffer wichtiger macht.
- **Bilder überlappen (Pipeline):** CPU, Grafik und Anzeige arbeiten gleichzeitig an Bild
  N+1, N und N−1, die Zeit pro Bild ist das Maximum statt der Summe. Kosten: ein Bild mehr
  Verzögerung zwischen Eingabe und Anzeige (16,7 ms bei 60 fps).
- **Zwei Framebuffer im Block-RAM** statt im PSRAM: Der Rasterizer schreibt den hinteren
  mit einem Pixel pro Takt, Übermalen kostet nichts, und es braucht weder PSRAM-Controller
  noch Arbitrierung für den Framebuffer (spart Logik, die knappe Ressource). Jeder Puffer
  hat seine eigene Palette, Palettenwechsel (Überblendungen) passen damit genau zum Bild.
  Getauscht wird nur zu Beginn eines Bildes, in der Austastlücke (`video_out.v`). Auf dem
  Pocket getestet.
- **SRAM für die Befehlslisten:** AS6C2016-55, 55 ns, laut Analogue kaum schneller als der
  PSRAM und ohne Bursts, also nichts für Pixel. Für die doppelt gepufferten Objektlisten
  (~20–30 KB je Bild, ~1–2 MB/s) reicht es gut, und die Geometrie-Einheit liest sie über
  einen eigenen Bus, ohne die CPU im SDRAM zu stören.

## Offene Entscheidungen

- **Perspektivkorrektur:** pro Pixel oder alle 8/16 Pixel. Entscheidung am C-Modell.
- **Zahlenformate:** Rasterizer und Geometrie in Festkomma (Bildschirmkoordinaten mit
  mindestens 4 Nachkomma-Bits), die Physik auf der CPU in `float` wie im Original.

## Vorgehen

1. Grundgerüst: Template, zwei Framebuffer mit Palette, Eingabe (erledigt, auf dem Pocket
   getestet); JTOPL fehlt noch.
2. VexRiscv mit FPU und SDRAM. Das erste Programm ist ein Speichertest: SDRAM, SRAM und
   PSRAM mit Mustern beschreiben, zurücklesen, Ergebnis auf dem Bildschirm. Damit werden
   die externen Speicher geprüft, bevor etwas auf sie baut (das SRAM erst hier, nicht
   vorher einzeln). Danach die Spiellogik von OpenScreamer bare-metal, erst mit
   Software-Renderer.
3. Bitgenaue C-Modelle von Rasterizer und Geometrie-Einheit, dann die Hardware dazu,
   jeweils in Verilator gegen das Modell geprüft.

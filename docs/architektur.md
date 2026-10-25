# Architektur

Stand: 2026-10-25. Gebaut und auf dem Pocket getestet: Video-Ausgabe 320×200 aus zwei
8-Bit-Framebuffern im Block-RAM, je mit eigener Palette, getauscht in der Austastlücke, und
die CPU (VexRiscv RV32IMF mit FPU und CFU, `docs/soc.md`), die in Schritt 1 selbst in die
Framebuffer zeichnet und ein Protokoll auf die SD-Karte schreibt. Belegung: 3.679 ALMs,
176 M10K, 11 DSP-Blöcke. Davor lief der Framebuffer im PSRAM `cram1` (Commit `b148783`).
Alles Weitere sind Schätzungen, bis die Synthese echte Zahlen liefert.

## Hardware des Pocket (Haupt-FPGA)

Cyclone V 5CEBA4F23C8: 18.480 ALMs, 308 M10K (je 10 Kbit), 66 DSP-Blöcke, 4 PLLs.
Getrennte Speicherbusse: SDRAM 64 MB, PSRAM `cram0` und `cram1` je 16 MB, SRAM 256 KB
(alle 16 Bit breit).

## Aufteilung

![Geplante Architektur: Die CPU schreibt die Objektliste von Bild N+1 ins SRAM, die Geometrie-Einheit liest die von Bild N und gibt Dreiecke an den Rasterizer, der Bild N in den hinteren Framebuffer im Block-RAM zeichnet, während video_out den vorderen zeigt](architektur.svg)

Grün: gebaut und auf dem Pocket getestet. Grau gestrichelt: geplant. Drei Bilder sind
gleichzeitig in Arbeit: Die CPU berechnet Bild N+1 und schreibt dessen nach Abstand
sortierte Objektliste ins SRAM, die Geometrie-Einheit liest die Liste von Bild N und
liefert Dreiecke an den Rasterizer, der Texel aus `cram0` liest und Bild N in den
hinteren Framebuffer zeichnet, während `video_out` den vorderen zeigt (Bild N−1).
Getauscht wird in der Austastlücke; Befehlslisten und Framebuffer sind doppelt gepuffert.

| Speicher | Inhalt |
|---|---|
| SDRAM | Programm, Geometrie, Spielzustand, Sounds |
| SRAM | Befehlslisten der Geometrie-Einheit, doppelt gepuffert (je ≤ 26 KB), Rest Reserve |
| cram0 | Texturen der aktuellen Strecke (`MAPn.MLB`, ≤ 3 MB), später im Burst-Modus |
| cram1 | frei |
| Block-RAM | zwei Framebuffer (2 × 64 KB) und zwei Paletten, Boot-RAM, CPU-Caches, Textur-Cache, Transparenztabelle (`MAPn.SMG`) |

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

![Platzbudget im FPGA: Logik geplant 55–77 %, Block-RAM 67–74 %, DSP 42–61 %](ressourcen.svg)

Die Grafik erzeugt `tools/make_budget_svg.py` aus denselben Zahlen wie diese Tabelle; wenn
die Synthese eine Schätzung ersetzt, beides anpassen.

| Komponente | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue-Rahmen (**gemessen**, unverändertes Template) | 414 | 2 | 0 |
| Framebuffer ×2 + Video (**gemessen**: `framebuffer`, `video_out`, `fb_bridge`) | 78 | 130 | 0 |
| VexRiscv RV32IM + 2× 8 KB Cache (einzeln synthetisiert: 1.268–1.278) | 1.250–1.300 | 19 | 4 |
| FPU (RV32F, einzeln synthetisiert: ~1.780) | 1.750–1.800 | 5 | 4 |
| CFU (eigene Befehle, z. B. 16.16-Multiplikation) | 30–100 | – | 3 |
| Boot-RAM 4–16 KB (Startcode, Speichertest) | 20–50 | 4–16 | – |
| Speicher-Controller (SDRAM, `cram0`) + Arbiter | 900–1.400 | 2–4 | 0 |
| SRAM-Controller (Befehlslisten) | 100–200 | – | 0 |
| Geometrie-Einheit | 2.500–4.000 | 4–8 | 12–18 |
| Rasterizer (Logik) | 2.000–3.000 | 3–6 | 4–8 |
| Textur-Cache 16 KB | 100–200 | 16 | – |
| Transparenztabelle (`MAPn.SMG`, 64 × 256 Byte) | – | 16 | – |
| JTOPL | 800–1.200 | 3–5 | 0–1 |
| PCM-Mixer | 300–500 | 2 | 1–2 |
| **Summe** | **10.242–14.242 (55–77 %)** | **206–229 (67–74 %)** | **28–40 (42–61 %)** |

Knapp wird die Logik: ab etwa 80 % Belegung wird es auf dem Cyclone V schwer, die
Taktfrequenz zu halten. Block-RAM trägt die beiden Framebuffer (128 M10K); CPU-Caches und
Textur-Cache teilen sich den Rest und werden später an Messungen ausgerichtet. Einen
Tiefenpuffer gibt es nicht, siehe „Entschieden“.

VexRiscv und FPU stammen aus einer Synthese der CPU allein für den 5CEBA4F23C8 mit Quartus
18.1.1 (Recherche 2026-10-25): RV32IM mit 2 × 8 KB Cache 1.268–1.278 ALMs, 19 M10K, 4 DSP,
Fmax um 115 MHz; mit FPU 3.052–3.061 ALMs, 24 M10K, 8 DSP, Fmax 98–111 MHz (Median 105).
Die FPU macht Float-Rechnungen 13–32 × schneller als Soft-Float.

## Entschieden

- **Sprache:** eigener Code in Verilog (wie das Template). Der VexRiscv wird einmalig aus
  einer Scala-Konfiguration als Verilog erzeugt, eigene CPU-Befehle laufen über sein
  CFU-Plugin. Gleiche Leistung wie SpinalHDL, leichter zu lesen.
- **Spieldaten:** Originaldateien flach in `Assets/screamer/common/` (siehe oben).
- **VGA-Fassung:** 320×200 mit 8-Bit-Palette wie die VGA-Version des Spiels, daher nur die
  L-Dateien. Cockpit, Renninfos und andere statische Bilder sind Sprites aus dem L-Satz von
  `GAME.DAT` und kommen 1:1 pixelgenau in den Framebuffer: erst von der CPU gezeichnet,
  später über einen Sprite-Modus des Rasterizers (Rechteck unverzerrt kopieren, eine Farbe
  durchsichtig).
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
  Ebene. Es spart rund 125 M10K (320×200×16 Bit passen nur in die 8-Kbit-Modi); dafür
  wird jedes verdeckte Pixel erneut geschrieben, was die Bandbreite zum Framebuffer
  wichtiger macht.
- **Bilder überlappen (Pipeline):** CPU, Grafik und Anzeige arbeiten gleichzeitig an Bild
  N+1, N und N−1, die Zeit pro Bild ist das Maximum statt der Summe. Kosten: ein Bild mehr
  Verzögerung zwischen Eingabe und Anzeige (16,7 ms bei 60 fps).
- **Zwei Framebuffer im Block-RAM** statt im PSRAM: Der Rasterizer schreibt den hinteren
  mit einem Pixel pro Takt, Übermalen kostet nichts, und es braucht weder PSRAM-Controller
  noch Arbitrierung für den Framebuffer (spart Logik, die knappe Ressource). Jeder Puffer
  hat seine eigene Palette, Palettenwechsel (Überblendungen) passen damit genau zum Bild.
  Die Zeichenseite liest auch (Rauch, Glas, Schatten und Radar verrechnen den Bildinhalt):
  vier explizite 8-Bit-Speicherbahnen mit Lese-/Schreibport (`fb_lane.v`), weil Quartus
  18.1 den Speicher mit echten Byte-Enables sonst stillschweigend aus Registern baut;
  `tools/check_budget.py` prüft bei jedem Build die 128 M10K. Getauscht wird am Anfang
  jeder Zeile der Austastlücke, höchstens einmal je Lücke (`video_out.v`): Ein Bild, das
  etwas länger als 16,7 ms braucht, fällt so nicht gleich auf 30 fps. Unabhängig
  gegengeprüft gegen SRAM, PSRAM und Mischformen (2026-10-25).
- **SRAM für die Befehlslisten:** AS6C2016-55, 55 ns, laut Analogue kaum schneller als der
  PSRAM und ohne Bursts, also nichts für Pixel. Für die doppelt gepufferten Objektlisten
  (je ≤ 26 KB, ~10 % der SRAM-Zeit) reicht es gut, und die Geometrie-Einheit liest sie
  über einen eigenen Bus, ohne die CPU im SDRAM zu stören. Den Listenzugriff so bauen,
  dass die Listen in den SDRAM umziehen können, falls die Logik knapp wird.
- **Perspektivkorrektur exakt pro Pixel:** Eine Kehrwert-Einheit (Tabelle und ein
  Newton-Schritt, 2–3 DSP) liefert einen Kehrwert pro Takt, also kostet es keine Zeit. Der
  Software-Renderer von OpenScreamer rechnet nur jedes achte Pixel exakt, daher seine Knicke
  in nahen Texturen. Die Festkomma-Bitbreiten werden am C-Modell gegen Fließkomma bestimmt.

## Übergabe zwischen den Bildern (Vorgaben für CPU und Grafik)

Aus der Gegenprüfung der Framebuffer-Platzierung:

- Der Tausch wird erst angefordert, wenn der letzte Pixel des Bildes geschrieben ist:
  Ein Ende-Zeichen läuft durch die ganze Grafik-Pipeline, erst danach kippt
  `swap_req_toggle`.
- Palettenwechsel stehen in der Befehlsliste von Bild N und werden beim Beginn von Bild N
  in die Palette des hinteren Puffers geschrieben. Die CPU schreibt die Palette nie direkt,
  denn sie ist schon ein Bild weiter.
- Der Zeichner kennt seinen hinteren Puffer aus seinem eigenen Toggle (hinten = nicht
  `req`) und wartet nur auf die Quittung; er liest `front` nicht über die Taktdomäne.
- Ein Reset darf keinen Tausch vortäuschen: Er setzt Anforderung und Quittung auf 0, danach
  ist Puffer 0 vorn, bis der Zeichner tauscht (in `sim/display` und `sim/soc` geprüft).

## Offene Entscheidungen

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

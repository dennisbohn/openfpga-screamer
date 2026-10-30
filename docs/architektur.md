# Architektur

Stand: 2026-10-30. Gebaut und auf dem Pocket getestet: Video-Ausgabe 320×200 aus zwei
8-Bit-Framebuffern im Block-RAM, je mit eigener Palette, getauscht in der Austastlücke; die
CPU (VexRiscv RV32IMF mit FPU und CFU, `docs/soc.md`); Geometrie-Einheit, Clipper,
Setup-Einheit und Rasterizer mit vorausladendem Textur-Cache; Strecke 1 mit dem Auto des
Spielers, fahrbar, mit dem HUD aus dem Blitter, der Füllregel des Originals (keine Lücken
an Fugen), dem Hintergrundbild `MAPn.BCK`, neun Gegnern (Startaufstellung, ihre Linien,
Platzierung, Berührungen, Überholen, Crash eines Gegners), der schnelleren Grafik-Kette
(50 fps auch am Start mit allen zehn Autos) und beweglichen Teilen der Strecke (der drehende
Football-Helm am Start); die Strecke wird aus den originalen Spieldateien gebaut. Gebaut, Test
auf dem Pocket steht aus: Routine 224 (Rauch, Schatten unter allen Autos), das Radar, die
Cockpit-Ansicht, die Startrichtung des Originals und der Ton: ein Mischer mit 32 Stimmen
(`audio.v`), der die Soundeffekte aus `SOUND.DAT` spielt (Motor, Rutschen, Wand, Crash,
Requisiten, Berührung mit einem Gegner, die Motoren der Gegner und die Fahrzeuge der Strecke,
im Stereobild und mit Doppler) und den Sprecher. Seit 2026-10-30 gebaut, Test auf dem Pocket
steht aus: alle sechs Strecken, gewählt im Streckenbildschirm des Originals (siehe
„Streckenwahl“), und die CD-Musik aus GOGs Abbild der CD, im Streckenbildschirm und im Rennen
wie im Spiel.
Belegung siehe „Belegung“, alles gemessen.

## Hardware des Pocket (Haupt-FPGA)

Cyclone V 5CEBA4F23C8: 18.480 ALMs, 308 M10K (je 10 Kbit), 66 DSP-Blöcke, 4 PLLs.
Getrennte Speicherbusse: SDRAM 64 MB, PSRAM `cram0` und `cram1` je 16 MB, SRAM 256 KB
(alle 16 Bit breit). Benutzt wird nur der SDRAM; SRAM und PSRAM sind seit 2026-10-26
ausgebaut (nur der Speichertest brauchte sie).

## Aufteilung

![Architektur: Die CPU wählt und sortiert die Bauteile und schreibt sie in den Puffer der Geometrie-Einheit; Geometrie-Einheit, Clipper, Setup-Einheit und Rasterizer zeichnen das Bild in den hinteren Framebuffer im Block-RAM, während video_out den vorderen zeigt](architektur.svg)

Wie der Rasterizer ein Dreieck in Pixel zerlegt und wie der Texturpfad als Fließband geplant ist: [rasterizer.svg](rasterizer.svg).
Wo die Zeit eines Bildes bleibt, CPU-Abschnitte und Auslastung der Einheiten über 20 ms (aus dem Protokoll gezeichnet von `tools/make_timeline_svg.py`, auch aus der `debug.txt` des Pocket): [bildablauf.svg](bildablauf.svg).

Grün: gebaut und auf dem Pocket getestet. Grau gestrichelt: geplant. Die CPU wählt die
Bauteile in Reichweite, sortiert sie von hinten nach vorn und schreibt sie in den Puffer der
Geometrie-Einheit; Geometrie-Einheit, Clipper, Setup-Einheit und Rasterizer arbeiten
gleichzeitig daran, der Rasterizer liest Texel über seinen Textur-Cache aus dem SDRAM und
zeichnet in den hinteren Framebuffer, während `video_out` den vorderen zeigt. Getauscht wird
in der Austastlücke. Geplant: Die CPU rechnet schon Bild N+1, während die Hardware Bild N
zeichnet.

| Speicher | Inhalt |
|---|---|
| SDRAM | Programm, Geometrie, Spielzustand, Texturen der aktuellen Strecke (`MAPn.MLB`, ≤ 3 MB, über den 16-KB-Textur-Cache) |
| SDRAM | Soundeffekte (`SOUND.DAT`, 1,9 MB, ungepackt; der Mischer liest die Samples direkt) |
| Block-RAM | zwei Framebuffer (2 × 64 KB) und zwei Paletten, Boot-RAM, CPU-Caches, Puffer der Geometrie-Einheit, Textur- und Mesh-Cache, Transparenztabelle (`MAPn.SMG`), die Zeilen des Mischers (32 × 32 Bytes) |
| SRAM, cram0, cram1 | nicht benutzt |

Eine Cache-Zeile Texturen kostet aus dem SDRAM etwa 30 Takte, aus dem PSRAM (asynchron)
etwa 130; deshalb liegt alles im SDRAM, auch die Sounds, die der Mischer gleichmäßig und
langsam liest. Je Stimme hält der Mischer die Zeile von 32 Bytes um ihre Stelle im Block-RAM
und fordert die nächste an, sobald die Stelle hineinrückt; bis er sie liest, vergeht ein ganzer
Ausgabewert (1547 Takte), das fängt jede Wartezeit ab.

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

Die originalen Dateien, unverändert in ihren Ordnern unter `Assets/screamer/common/`: die
Ordner `DATA`, `GFX` und `TABS` des Spiels so, wie sie sind (der Pocket nimmt den Ordner im
Dateinamen eines Slots an, `GFX/MAP1H.DAT`, auf dem Pocket getestet 2026-10-28). Je Datei
ein Data-Slot mit `deferload`:
der Core liest das Inhaltsverzeichnis am Dateiende und holt sich die Einträge, die er
braucht. Gebaut (2026-10-28): die fünf Dateien von Strecke 1 (Slots 3 bis 7: `MAP1H.DAT`,
`GAME.DAT`, `MAP1.DAT`, `GEN.DAT`, `LOCAL.DAT`), seit 2026-10-29 dazu `SOUND.DAT` (Slot 8,
ungepackt, der Mischer spielt die Samples aus dem Container); die Firmware entpackt sie (`dat.c`, LZW)
und baut daraus beim Start das Abbild der Strecke, das bis dahin `tools/make_track.py` auf
dem PC baute, Byte für Byte dasselbe (`track_build.c`; Simulation: 3,2 s, davon 1 s
Entpacken, der Rest vor allem der Kamerapfad in `double` und die Prüfsummen). Alle Dateien
wären 33, eine mehr als die 32 Slots; da wir auf HI laufen, fallen die sechs L-Strecken
weg, und mit der CD-Musik `MUSIC.DAT`, dafür kommen `SCREAM.gog` und `SCREAM.DAT` dazu.
Seit 2026-10-30 sind es 18 Spieldateien: die Container der Strecken 2 bis 6 (Slots 9 bis 13
`MAP2H.DAT`–`MAP6H.DAT`, 14 bis 18 `MAP2.DAT`–`MAP6.DAT`, an dieselben Adressen wie die von
Strecke 1, geladen erst, wenn die Strecke gewählt ist) und für den Streckenbildschirm
`GFX/MENU.DAT` (Slot 19) und `GFX/GRAF.DAT` (Slot 20). Von diesen beiden liest der Core nur
das Inhaltsverzeichnis und die Einträge, die er braucht, jeden an seiner Stelle wie in der
Datei (`load_parts` in `main.c`): 0,7 MB statt 5,9 MB, bei den gemessenen 1,3 MB/s des
Pocket rund eine halbe statt viereinhalb Sekunden beim Start. `GAME.DAT`, `GEN.DAT`, `LOCAL.DAT` und `SOUND.DAT` lädt er einmal.
Dazu seit 2026-10-30 GOGs Abbild der CD oben im Spielordner: `SCREAM.DAT` (Slot 21, das
Cue-Sheet) und `SCREAM.gog` (Slot 22, 383 MB); daraus liest der Core nur die Musik, 32 KB auf
einmal, während er zeichnet (siehe „Die CD-Musik“).

- `DATA/`: alle 16 (`GEN`, `LOCAL`, `MUSIC`, `SOUND`, `MAP1`–`MAP6`, `MAP1I`–`MAP6I`)
- `GFX/`: `GAME`, `GRAF`, `MENU`, `MAP1L`–`MAP6L`, `MAP1H`–`MAP6H` (L und H sind die Sätze der
  Option DETAIL: L bei LOW, H ab MID; nicht die Auflösung)
- `TABS/`: `PERSP01.TAB`, `VIS.TAB`

## Streckenwahl

Wie im Original (sein Bildschirm 3: `0x2e384`, `0x2c078`, `0x2cb88`, `0x29530`; OpenScreamer
`docs/NOTES.md` Abschnitte 18 und 26), einfachste Fassung in `firmware/menu.c`: Die sechs
Modelle `AMAPn.3DX` aus `MENU.DAT` stehen in einer Reihe, 40.000 Einheiten auseinander, und
drehen sich (π/4 rad/s); die Kamera schaut aus (−40.000·n, 4800, 13.000) 36° hinab und
gleitet bei links/rechts zum Nachbarn (Faktor 1 → 0 mit 0,95/s, Weg mit der vierten Potenz,
der nächste Druck zählt erst unter 0,8). Die Modelle gehen durch die Geometrie-Einheit wie
Objekte im Rennen, mit `MENU.MLB` als Materialdatei (alle ihre Flächen sind einfarbig,
Routine 128, also reichen die ersten 32 KB); jeder Block an seinem Platz aus dem
Positionssatz, der fernste zuerst. Davor das Bild `MENU3L.PCX` aus `GRAF.DAT` (seine Palette
ist die des Bildschirms), darüber Titel (`MENU.TXT` Zeile 45, Schrift 3), Streckenname
(`TRACKDAT.TXT`, Schrift 2, 2 Schrifthöhen über dem unteren Rand) und die Pfeile (Sprites 0
und 1 bei x 8 und 238, y 100) aus `MENUL.FNT` und `MENUL.SPR`, alles über den Blitter. B oder
START startet; danach lädt, entpackt und baut der Core die Strecke (einige Sekunden; Strecke 1
auf dem Pocket: 1,2 s Laden, 3,2 s Bauen; das letzte Bild bleibt so lange stehen). SELECT im Rennen oder 5 s nach seinem Ende führt zurück.
Nicht dabei: Autowahl, Hauptmenü, Optionen, Intro, Kameraflüge, Menügeräusche, die
gespiegelten Strecken, „NEW TRACK AVAILABLE“. Die Simulation fährt gleich die Strecke aus
`SIM_TRACK` (Slot 28); mit `SIM_MENU=1` (Slot 27) zeigt sie stattdessen den
Streckenbildschirm und prüft sein drittes Bild gegen die C-Modelle (2026-10-30: 0 Pixel
Unterschied). `TRACK=n` und `MENU=1` für `tools/render.sh` zeichnen Start oder
Streckenbildschirm auf dem PC.

Die sechs Startbilder (Simulation, Schnelllauf, nahe Verfolgerkamera, Bild 2; jeweils 0 Pixel
Unterschied zu den C-Modellen; „Bild“ ist die ganze Zeit von Bild 1, 20 ms = 1,485 M Takte):

| Strecke | Platzierungen | Bauteile | Flächen | Pixel | Bild |
|---|---|---|---|---|---|
| 1 Palm Town | 1164 | 260 | 2620 | 108.027 | 1.315.412 = 17,7 ms |
| 2 Lake Valley | 1408 | 243 | 2411 | 113.588 | 1.206.304 = 16,2 ms |
| 3 Lindburg | 1590 | 235 | 2283 | 98.100 | 1.151.696 = 15,5 ms |
| 4 Palm Town Night | 1764 | 260 | 2763 | 112.283 | 1.363.361 = 18,4 ms |
| 5 Sunbeach Hill | 2359 | 249 | 3166 | 120.056 | 1.295.826 = 17,5 ms |
| 6 Sandrock | 2073 | 265 | 2502 | 132.668 | 1.354.951 = 18,2 ms |

Die Strecken 5 und 6 haben mehr Platzierungen als die 2048, die `track.c` hielt (jetzt 2560).
Strecke 5 stellt keine Requisiten auf: die Knoten von `MAP5OST.SPL` liegen nicht über ihrer
Fahrbahn (offen, am Original zu prüfen).

## Belegung

Gemessen am 2026-10-30 (Quartus-Build mit der CD-Musik, Timing hält bei 74,25 MHz: Setup
+0,94 ns, Hold +0,06 ns; Bauzeit 12 min): 10.278 von 18.480 ALMs (56 %), 281 von 308 M10K
(91 %, davon 16 die Tabelle der Routine 224), 31 von 66 DSP (47 %). Der Mischer mit der Musik:
565 ALMs, 1 M10K, 1 DSP; die Musik kostet 149 ALMs (ihre zwei Zeilen im MLAB, Register,
Interpolation), kein M10K und kein DSP (sie rechnet auf dem Multiplizierer der Stimmen). Vorher
(2026-10-29, nur die Effekte): 10.114 ALMs, Mischer 416, Setup +0,28 ns, Hold +0,10 ns; davor
9.683 ALMs, 280 M10K, 30 DSP, Setup +0,94 ns. Von Build zu Build
schwankt die Logik um einige hundert ALMs: Die Platzierung packt verschieden dicht, und wo
das Timing knapp ist, baut der Fitter Logik und Register um (Physical Synthesis).

**Weniger Logik (2026-10-28/29), von 15.646 auf 9.674 ALMs, ohne dass sich ein Ergebnis
ändert:** (1) Die Projektdatei aus der Vorlage von Analogue stellte die Synthese auf Tempo:
`OPTIMIZATION_TECHNIQUE SPEED` und `MUX_RESTRUCTURE OFF`. Ohne Umbau der Auswahlschaltungen
wurde aus jedem breiten Register, das in vielen Zuständen eines Ablaufs nur gehalten wird, und
aus jeder Teilauswahl mit zu breitem Index eine eigene breite Auswahl. Mit `BALANCED` und
`MUX_RESTRUCTURE ON` sank die Logik auf 14.046 ALMs (76 %), das Timing wurde sogar besser
(gemessen in zwei vollen Builds mit `AUTO` und `ON`). (2) Der Clipper ist neu geschrieben
und rechnet Takt für Takt dasselbe (gegen den alten mit 90.000 Zufalls-Vielecken geprüft, in
der Simulation 0 Pixel Unterschied und dieselben Takte): Die 4-Bit-Kantennummer wählte aus
dem 576-Bit-Eingang wie ein Schieber über 16 Plätze (allein 1.101 ALUTs), eine Ecke wurde
zweimal gehalten und zweimal gemessen, das Ende der Kante kommt jetzt aus dem Puffer, der
Abstand zur Ebene ist ein Addierer mit gewählten Operanden, ein Schieber statt zweien. Jetzt
928 statt 2.807 ALMs. (3) Setup-Einheit, Geometrie-Einheit und Rasterizer ebenso (je in
der Einzelsynthese gemessen, Takt für Takt gegen die alte Fassung geprüft mit 30.000 bis
200.000 Zufallsfällen, in der Simulation dieselben Pixel in denselben Takten): breite Register
mit Freigabe und kleinen Auswahlen statt gehalten in jedem Schritt, ein gemeinsamer Addierer
für alle Lesezugriffe bzw. Zähler, Matrix, Ursprung und Auge des Bauteils und die Register
7 bis 18 des Rasterizers in kleinen Block-RAMs, schmalere Werte, wo die Ecken auf ganzen
Pixeln liegen; den 576-Bit-Eingangspuffer der Setup-Einheit in Registern gelassen (im
Block-RAM spart er 190 ALMs, kostet aber 19 M10K, die knappere Ressource). Setup-Einheit
3.041 → 1.512, Geometrie-Einheit 2.288 → 648, Rasterizer mit Texturen 2.539 → 1.895 ALMs; dafür
11 M10K mehr und 5 DSP weniger. Geschätzt ist nur, was noch fehlt.

![Platzbudget im FPGA: Logik 56 %, Block-RAM 91 %, DSP 47 %](ressourcen.svg)

Die Grafik erzeugt `tools/make_budget_svg.py`: die gemessenen Zeilen aus dem Fitter-Report
(`core/src/fpga/output_files/ap_core.fit.rpt`, Tabelle „Resource Utilization by Entity“), die
geplanten aus Schätzungen im Skript. Nach jedem Build neu erzeugen; das Skript schreibt auch
diese Tabelle.

| Komponente | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue-Rahmen (Bridge, PLLs) | 417 | 2 | – |
| Framebuffer ×2 + Video | 122 | 130 | – |
| VexRiscv + 2× 8 KB Cache | 1.280 | 19 | 4 |
| FPU (RV32F) | 1.450 | 5 | 4 |
| CFU (16.16-Multiplikation) | 9 | – | 3 |
| Boot-RAM 32 KB + Protokoll | – | 36 | – |
| Geometrie-Einheit | 692 | 22 | 4 |
| Clipper | 943 | 6 | 2 |
| Setup-Einheit | 1.511 | 7 | 4 |
| Rasterizer | 1.260 | 2 | 5 |
| Texturen: Pipeline + Cache | 695 | 47 | 4 |
| Blitter (Hintergrund, HUD) | 509 | 2 | – |
| SDRAM-Controller + Ports | 305 | 2 | – |
| Audio: Mischer 32 Stimmen + Musik, I²S | 572 | 1 | 1 |
| Bus, Register, Arbiter | 556 | – | – |
| **gebaut** | **10.321 (56 %)** | **281 (91 %)** | **31 (47 %)** |

Knapp wird die Logik: ab etwa 80 % Belegung wird es auf dem Cyclone V schwer, die
Taktfrequenz zu halten. Das zeigt sich beim Bauen (Timing-Analyse, `tools/build.sh` meldet es
und probiert weitere Fitter-Seeds), nicht als Instabilität auf dem Pocket, solange das Timing
hält. Die größten Posten sind heute Setup-Einheit und FPU (je rund 1.500 ALMs). Die
Setup-Einheit rechnet bit-genau wie das C-Modell in Festkomma, deshalb breit: eine Ecke hat
144 Bit, ein Polygon 576, Zwischenwerte und Produkte bis 65 Bit. Es sind drei Rechenwerke
hintereinander, damit sie gleichzeitig arbeiten (Clipper, Ecken, Dreiecke), jedes mit einem
Multiplizierer und einem festen Ablauf: Vor jedem Multiplizierer sitzt eine Auswahl, beim
Dreiecksteil 47 Bit breit aus 28 Rechenschritten, dahinter Register-Felder für die
Zwischenwerte (rund 1.100 Bit), die wieder ausgewählt werden. Dazu kommen variable Schieber
für das Normieren vor der Kehrwerttabelle und das Skalieren der Ergebnisse (die
Setup-Einheit hat acht, 32 bis 65 Bit breit, der Clipper drei, je einige hundert ALUTs), und
Produkte breiter als ein DSP-Block (47 × 18, 38 × 18) addieren ihre Teilprodukte in Logik.
Vor der Verkleinerung waren es 3.041 ALMs, siehe oben.
Einen Tiefenpuffer gibt es nicht, siehe „Entschieden“.

VexRiscv und FPU stammen aus einer Synthese der CPU allein für den 5CEBA4F23C8 mit Quartus
18.1.1 (Recherche 2026-10-25): RV32IM mit 2 × 8 KB Cache 1.268–1.278 ALMs, 19 M10K, 4 DSP,
Fmax um 115 MHz; mit FPU 3.052–3.061 ALMs, 24 M10K, 8 DSP, Fmax 98–111 MHz (Median 105).
Die FPU macht Float-Rechnungen 13–32 × schneller als Soft-Float.

## Entschieden

- **Sprache:** eigener Code in Verilog (wie das Template). Der VexRiscv wird einmalig aus
  einer Scala-Konfiguration als Verilog erzeugt, eigene CPU-Befehle laufen über sein
  CFU-Plugin. Gleiche Leistung wie SpinalHDL, leichter zu lesen.
- **Spieldaten:** die Ordner des Spiels unverändert in `Assets/screamer/common/` (siehe oben).
- **50 Hz, ein Spielschritt pro Bild:** Das Original hat keinen festen Takt, seine Physik
  rechnet mit der gemessenen Bildzeit, begrenzt auf 0,02–0,2 s; schneller als 50 Schritte
  pro Sekunde wird es also nie (OpenScreamer `docs/NOTES.md`, Fahrmodell). Die Bildausgabe
  läuft deshalb mit genau 50 Hz (480 × 512 Takte bei 12,288 MHz, der Bildschirm des Pocket
  kann 47–61 Hz), und jedes Bild ist ein Physikschritt mit 20 ms: das Spiel bei seiner
  besten Rate, ohne Zwischenbilder und ohne Ruckeln. Die Uhr für Countdowns und
  Rundenzeiten tickt im Original mit 100 Hz und bekommt ein eigenes Zählregister. Pro Bild
  stehen 1,485 Millionen Takte zur Verfügung.
- **VGA-Fassung:** 320×200 mit 8-Bit-Palette wie die VGA-Version des Spiels. Die Strecken
  kommen trotzdem aus beiden Sätzen: `MAPnL` bei DETAIL LOW, `MAPnH` ab MID (STARTL.EXE wie
  STARTH.EXE, OpenScreamer `docs/NOTES.md` Abschnitt 27). Wie weit gezeichnet wird, legt DETAIL
  fest wie im Original: Teile bis 52000 Einheiten, Flächen geschnitten in der Tiefe 27808
  (LOW, MID) oder 43808 (HI). Die SVGA-Fassung zeichnet genauso weit (2026-10-28 in
  `STARTH.EXE` nachgelesen: dieselbe Tabelle bei `ds:0x21d8`, derselbe Vergleich mit
  52000²/4 bei `0x8340b`); sie verdoppelt nur die Pixel (Brennweite 128, das Ergebnis × 2,
  Mitte Spalte 319), Blickwinkel und Sichtweite bleiben. Cockpit, Renninfos und andere statische Bilder sind Sprites aus dem L-Satz von
  `GAME.DAT` und kommen 1:1 pixelgenau in den Framebuffer: erst von der CPU gezeichnet,
  später über einen Sprite-Modus des Rasterizers (Rechteck unverzerrt kopieren, eine Farbe
  durchsichtig).
- **Sichtbarkeit wie im Original, ohne Tiefenpuffer** (Maler-Algorithmus): Die CPU
  sortiert ganze Objekte nach dem Abstand ihres Mittelpunkts von der Kamera und gibt sie
  von hinten nach vorn aus (im Original `0x83164`: `dist² >> 2`). Innerhalb eines Objekts
  liegt die Reihenfolge in den Daten: Jeder Mesh-Block hat sechzehn vorberechnete
  Flächenlisten (Header +0x28..+0x64), jede von hinten nach vorn sortiert für eine
  Blickrichtung: je ein 45-Grad-Sektor um die y-Achse des Blocks, für ein Auge über
  (Listen 0..7) und unter seiner x-z-Ebene (8..15). Die CPU wählt die Liste wie das Spiel
  (`0x8a598`; bis 2026-10-28 nur die oberen acht, daher der falsch verdeckte
  Gesichtsschutz des Helms am Start), die Geometrie-Einheit sortiert nicht und lässt
  Rückseiten über die Normale weg. Auge ist dabei wie im Spiel das Auge im Rahmen des Blocks
  plus dessen Mitte, mal 1,2 (`0x83511`, unskaliert bei einem 32-Bit-Überlauf): Dieser
  Punkt wählt die Liste und ist das Auge des Rückseitentests (`0x835a0`). Die Streckendaten verlassen sich auf diese Reihenfolge
  (Grasflächen unter der Straße, zwei Objekte an einer Stelle), OpenScreamer brauchte
  dafür mit seinem Tiefenpuffer Sonderregeln. Nötig bleibt das Clipping an der vorderen
  Ebene. Es spart rund 125 M10K (320×200×16 Bit passen nur in die 8-Kbit-Modi); dafür
  wird jedes verdeckte Pixel erneut geschrieben, was die Bandbreite zum Framebuffer
  wichtiger macht.
- **Erst die Setup-Einheit, dann der Rest der Geometrie:** Der Durchflug durch Strecke 1
  hat die Geometrie auf der CPU vermessen: rund 3000 Takte je Dreieck, fast die Hälfte
  davon die Vorbereitung je Dreieck (die Ebenen von Q, U, V mit Divisionen), ein Viertel
  Zuschneiden und Projektion, das Umrechnen der Eckpunkte nur 3 %. Deshalb kam zuerst die
  Setup-Einheit (`setup.v`, C-Modell `firmware/setup.c`): Die CPU gibt Vielecke in
  Kamerasicht ab (fünf Floats je Ecke), die Hardware projiziert, zerlegt sie in einen
  Fächer und rechnet die Werte für den Rasterizer in Festkomma, während der Rasterizer das
  vorige Dreieck zeichnet. Danach das Zuschneiden (`clipper.v`, davor): mit der
  Setup-Einheit sank die CPU-Zeit im schwersten Bild von 10,4 auf 5,8 Mio. Takte, davon
  58 % Zuschneiden. Wie im Original an der nahen Ebene (Tiefe 128) und der fernen der
  DETAIL-Stufe; dazu ein Schutzband bei 14 × Tiefe, nur damit die Bildschirmkoordinaten in
  16 Bit passen (den Bildrand schneidet der Rasterizer). Das Objektiv ist das des
  Originals: Brennweite 128, Augenhöhe bei 65 % der Bildhöhe, und das Auge 128 Einheiten
  hinter der Kamera (2026-10-29: die Teilung bei `0x83420` liest `TABS\PERSP01.TAB`, Bildpunkt
  = Mitte + 128 · x / (128 + z), z von der Kamera aus; bis dahin teilten wir durch z, das
  eigene Auto war um ein Fünftel zu groß). Danach blieben der CPU
  1,5–2,7 Mio. Takte je Bild, vor allem Flächenlisten und das Schreiben der Ecken. Die
  Geometrie-Einheit (`geom.v`, C-Modell `firmware/geom.c`) nimmt ihr das ab: Die CPU wählt
  die Bauteile aus, sortiert sie und schreibt je Bauteil 18 Wörter (Matrix, Ursprung,
  Auge, Mesh, Flächenliste) in den Puffer der Einheit (113 Bauteile, Block-RAM); die
  Einheit liest das Mesh selbst aus dem SDRAM, rechnet die Eckpunkte um, lässt Rückseiten
  weg, schlägt die Materialien nach und gibt die Flächen an den Clipper. Die CPU braucht
  etwa 20 % eines Bildes; durch den Puffer wartet sie nicht mehr auf jedes Bauteil, die
  Zeit bleibt für Autos und Spiellogik. Zuletzt kostete der Textur-Cache: Zähler im
  Rasterizer zeigten 76 % seiner Zeit als Warten auf Texturzeilen, eine Cache-Studie mit den
  Texel-Adressen des C-Modells halb so viele Fehltreffer wie gezählt: beide Caches holten
  jede fehlende Zeile zweimal. Behoben hält der Durchflug stabil 50 fps, das schwerste Bild
  braucht 73 % eines Bildes. Kacheln im Textur-Speicher brächten laut Studie kaum etwas
  (die meisten Fehltreffer sind Erstzugriffe, ~7.800 Zeilen je Bild). Deshalb holt der
  Rasterizer Texturzeilen voraus (nach Igehy, Eldridge, Proudfoot 1998): Die Tags werden
  vor einer Warteschlange von 64 Pixeln geprüft, eine fehlende Zeile wird sofort
  angefordert (bis 16 unterwegs, in einen Zeilenpuffer), und nur der Pixel, der sie
  braucht, wartet vorn in der Schlange; erst dann kommt sie in den Cache, wenn alle Pixel
  davor die alte Zeile gelesen haben. Das nächste texturierte Dreieck beginnt, sobald die
  Pixel des vorigen in der Warteschlange sind. Gemessen: Der Rasterizer ist im schwersten
  Bild 0,49 statt 0,65 Mio. Takte beschäftigt, das Bild braucht 64 % statt 73 % von 20 ms.
  Den Takt gibt jetzt die Geometrie-Einheit vor: Sie ist vom ersten Bauteil bis zum
  Bildende beschäftigt (einschließlich Warten auf Clipper und Speicher), die Bildzeit ist
  etwa Auswahl der CPU plus Geometrie-Einheit.
- **Das Auto des Spielers (2026-10-27):** Die Fahrphysik (Motor, Getriebe, Bremse, Lenkung,
  Rutschen) ist OpenScreamers Übertragung aus dem Spiel (`firmware/drive.c`), Zeile für Zeile;
  Bodenkontakt über die vier Räder, Wände, Marker und Kamera nach OpenScreamers Glue-Code
  (`firmware/car.c`), die Suche, was unter einem Punkt liegt, in `track.c` direkt auf den
  Blöcken im SDRAM. Das Auto selbst sind die zwölf Blöcke von `AUTOnn.3DX`, als Teile durch
  die Geometrie-Einheit, in der Reihenfolge des Spiels (Reifen, Naben, dann die Karosserie);
  nur die Naben drehen sich, die vorderen Blöcke lenken. Die Geometrie-Einheit wählt dazu das
  Bild einer Textur nach einer Phase je Teil (Lackierung, Bremslicht, Radbild; die
  Streckentexturen nach der Animationsuhr). Gas liegt auf B (unten), die Bremse auf Y (links),
  wie in den klassischen Rennspielen (Super Mario Kart, Gran Turismo), die Kamera auf X (oben);
  nicht auf dem Steuerkreuz, damit ein Lenken in die Diagonale nicht bremst.
- **Reihenfolge der Objekte wie im Original** (2026-10-27, aus `STARTL.EXE` gelesen: `0x83164`
  baut die Liste aus den Rasterzellen, je Zelle Lauf 1 vor Lauf 0, mit `dist² >> 2`;
  `0x8a6a8` hängt die bewegten Objekte an; `0x8a7b0` macht daraus den Schlüssel
  −(√(dist² >> 2) gerundet >> 2), also etwa Entfernung/8; `0x832c0` sortiert, gezeichnet wird
  das Fernste zuerst). Dabei zählt ein bewegtes Objekt (Auto) **800 näher**, 6400 Einheiten:
  Sonst übermalt das lange Straßenteil, auf dem es steht, das Auto, auf einer Runde mit der
  nahen Verfolgerkamera an jeder vierten Stelle. Und ein Mesh mit einer Klasse im letzten Wort
  seines Objektkopfs (+0x32c) zählt weiter weg: Klasse 3 um 6000 (auf Strecke 1 nur `akk1tap`,
  der Rasen unter der Straße), 1 um 1500, 2 um 800, 4 um 200. OpenScreamer zeichnet mit
  Tiefenpuffer und hat dafür eigene Regeln (Rasen „doppelt so weit“, Objekte über ihrer
  Straße); wir zeichnen wie das Original und brauchen nur dessen Schlüssel.
- **Der Crash wie im Original** (2026-10-27; Entscheidung `0x16c90` in der EXE gelesen, Ablauf
  `0x18518` nach OpenScreamers Notes): ein Wandtreffer ist ein Crash, wenn Tempo × (Winkel zur
  Straße / 90°) > 0,3 × Höchsttempo, aber nur im Freien (`+0x7b0`, die Art des letzten Markers,
  ist 0; im Tunnel prallt man nur ab), und die Wandreaktion kommt trotzdem (der Crash schaltet
  im Spiel nur den Wand-Ton ab). OpenScreamer prüft den Tunnel nicht und lässt beim Crash die
  Wandreaktion weg; wir folgen der EXE. Dann Tempo × 0,6, Richtung der Straße, hochgeworfen mit
  3000/s, 12 rad/s um Quer- und Längsachse, Schwerkraft 6000, vier Landungen, die Crash-Ansicht
  (Zeile 5 der Ansichten) und danach die alte.
- **Die Rennanzeige über einen Blitter** (2026-10-27): Das Original zeichnet sie in Software
  nach der Szene: Schrift aus `GAMEL.FNT` (Farbe × 16 + Schattierung), Sprites aus
  `GAMEL.SPR`, die Bilder `IMPLET_L`/`IMPRES_L` (.PKC), Nadeln als Linien, an den Plätzen von
  `0x23400`/`0x2367c`. Hier kopiert sie ein Blitter (`blit.v`) Pixel für Pixel 1:1 in den
  Framebuffer, ohne Geometrie, Setup und Rasterizer: Befehle mit x, y, w, h (Kopie, Schrift)
  oder als Linie, in einer Warteschlange hinter den Teilen des Bildes, ausgeführt, sobald die
  3D-Einheiten fertig sind. Die Bilder zerlegt `make_track.py` in Kästen um ihre Pixel, so
  dass der ganze HUD unter 1 % eines Bildes kostet; ein eigener HUD-Puffer (64 KB je Puffer)
  oder eine Maske für den Rasterizer brächten dafür zu wenig. Ampel, Countdown, Runden und
  Zeiten wie im Spiel (OpenScreamers `markers_step`, Notes Abschnitt 12).
- **Ecken auf ganzen Pixeln wie im Original** (2026-10-27, aus der EXE): Die Projektion
  (`0x83420`ff.) legt x als 16.16 an und behält nur das obere Halbwort, und alle Füllroutinen
  (`0xd5350` Texturen, `0xd8844` eine Farbe) lesen die Ecken als ganze Pixel aus der Tabelle
  bei `0x54e94`. Ecken benachbarter Teile, die in der Welt ein paar Einheiten auseinander
  liegen (die gespeicherten Winkel sind bis 2,5 Einheiten zu kurz), landen so meist auf
  demselben Pixel. Die Setup-Einheit und der Rasterizer schneiden deshalb die Ecken auf
  ganze Pixel ab, statt sie auf 1/16 Pixel zu runden.
- **Die Grafik-Kette schneller** (2026-10-28, gemessen in der SoC-Simulation mit Strecke und
  Zählern, `SIM_DEFINES="-DGEOM_STATS"`): Am Start mit allen zehn Autos im Bild (248 Bauteile,
  2.473 Flächen) brauchte ein Bild rund 1,6 Mio. Takte, mehr als die 1,485 Mio. eines Bildes:
  25 fps. Die Geometrie-Einheit war 1,15 Mio. Takte beschäftigt, davon 37 % mit Warten auf
  Lesezugriffe, die auch als Cache-Treffer 5 Takte kosteten, und 23 % mit SDRAM-Fehltreffern;
  der Rasterizer wartete 289.000 Takte auf Texturzeilen, die CPU 414.000 auf die Geometrie.
  Jetzt: (1) ein Lesezugriff der Geometrie-Einheit legt die Adresse direkt an die RAMs des
  Caches, der nächste Schritt nimmt den Wert (2 Takte bei einem Treffer, bei einem Fehltreffer
  wartet nur dieser Schritt); zwei weitere Multiplizierer rechnen die drei Zeilen einer Ecke
  zugleich (6 statt 12 Takte). (2) Der Zeilen-Port hat bis zu zwei Zeilen im SDRAM-Controller,
  der Textur-Lader fragt zwei an (`docs/speicher.md`). (3) Die Firmware baut die Autos erst,
  wenn sie in der Zeichenfolge dran sind (die nahen zuletzt, während die Geometrie-Einheit an
  der fernen Kulisse arbeitet), und überspringt beim Culling Gruppen von 16 Bauteilen, deren
  Umriss außer Reichweite liegt. Ergebnis: 996.000 Takte (67 %), die Geometrie-Einheit 812.000,
  der Rasterizer wartet 193.000 Takte auf Texturen, die CPU 28.000 auf die Geometrie. Größere
  Caches brächten wenig: eine Studie mit den Zugriffen des Startbilds (C-Modell) ergab für die
  Geometrie 8.738 Fehltreffer mit 4 KB, 6.368 mit 16 KB; für Texturen 11.200 mit 16 KB, 10.261
  mit 32 KB und mit 8 × 4 gekachelten Texturen sogar mehr (13.895): Die meisten sind die ersten
  Zugriffe auf rund 360 KB Textur- und 280 KB Meshdaten je Bild.
- **Die Gegner** (2026-10-27, nach OpenScreamer, dessen Adressen aus der EXE stammen; Notes
  Abschnitt 11): `MAPnOPP.SPL` hat zehn Splines, Gegner k fährt Spline k, der zehnte ist die
  Linie des Spielers; die Startknoten sind die Startaufstellung (zwei Spalten, der Spieler
  zuletzt, Platz 10). Die Kurve (`0x1d54c`, `0x1e7a4`) ist ein gleichmäßiger quadratischer
  B-Spline über die Griffe der Knoten. Ein Gegner lenkt nicht und rutscht nicht: Er fährt mit
  `max_spd` × PACE des zuletzt erreichten Markers (+ `skill`, solange der Spieler vor ihm
  liegt; `0x1ced0`), beschleunigt mit `accel`, bremst mit `brakes`, und rückt so weit auf
  seiner Kurve vor (`0x1d2fc`). Er steht auf einem Punkt, der Fahrbahn unter seiner Mitte,
  gekippt nach der Normalen dieser Fläche × 1,117 (`0x1cb9c`), dreht in Kurven die Nase ein
  und neigt die Karosserie (`0x1df24`, `sband`). Seine Marker und Runden zählt er wie der
  Spieler (`0x1db84`); die Platzierung (`0x20418`) tauscht je Schritt höchstens einen Gegner
  mit dem Spieler, sobald der über die Linie ist, Gegner untereinander nie. Die Autos sind die
  Zeilen von `CARS.DTR` nach der des Spielers (`0x142e8`). Die Werte kommen aus der Datei der
  Schwierigkeit (`'A'` + Stufe, `0x13251`); wir nehmen NORMAL (`MAP1B.DTR`, Bonuszeit 40 s statt
  50 s bei ROOKIE), die Stufe, die das Intro setzt (`0x2777f`). Gezeichnet wird ein Gegner nur,
  wenn seine Kugel (Radius 1200) im Blickfeld liegt. **Berührungen** (2026-10-28): Jedes Auto
  sind zwei Kreise auf der Mittellinie (`0x136c8`, `0x142e8`: die Karosserie von 250 vor der
  Vorderachse bis 150 hinter der Hinterachse, Länge L, jeder Kreis L/4 vom Ende und L/4
  groß). Spieler und Gegner (nur innerhalb 1500, `0x12378`; `0x17818`, Konstanten 0,5,
  0,0001, π/2, 0,21, 0,7 in der EXE nachgesehen): Der Gegner weicht um die Überlappung,
  außer das schöbe ihn von der Fahrbahn, dann weicht der Spieler; fährt der Spieler auf, wird
  der Gegner mit halbem Spielertempo weitergeschoben und angedreht, der Spieler mit 0,21 ×
  Tempo zurückgeschoben, um die Nase gedreht und verliert je Schritt 5 % Tempo (ein
  Wandtreffer); fährt der Gegner auf, behält er 70 %. Zwei Gegner (`0x1cffc`, `0x1d1f0`):
  Der schnellere fällt auf das Mittel, der spätere wird zur Seite geschoben. Überholen
  (`0x1e404`): Steht der langsamere Spieler drei Radien vor einem Gegner, verschiebt der
  seine Linie gerade so weit, dass er vorbeikommt. Ein Gegner, der mit mehr als 3000 von der
  Fahrbahn gedrängt wird, steht sofort (`0x1cb9c`); verliert er in einem Schritt mehr als 5000
  Einheiten/s, crasht er (`0x1d824`): je nach Stelle überschlägt er sich wie der Spieler oder
  dreht sich zweimal auf der Fahrbahn. Der Versatz aus Stößen klingt nach 3 s ab.
- **Die vier Ansichten des Spielers** (2026-10-29, aus der EXE, Tabelle `0x1da0`): Stoßstange,
  Cockpit, nahe und ferne Verfolgerkamera (X wechselt in dieser Reihenfolge, das Rennen
  beginnt nah, Option `0x1d2a4`). Das Cockpit ist die Stoßstangen-Kamera über einer 3D-Ansicht
  von 132 Zeilen (`0x4c610(320, 132, 85)`, `0x19cbe`) mit dem Armaturenbrett des Autos
  darunter: `COCKnnAL.PKC` (B für die zweite Lackierung, `0x13725`) aus `GAME.DAT`, beim Laden
  in Blitter-Kästen zerlegt, die Zahlen aus dem Abschnitt `COCKnnL` von `CARS.DTR` (`0x1545c`:
  Nadellänge, Drehpunkte, Lampen), Tacho- und Drehzahlnadel als je drei Linien in Farbe 255,
  geglättet wie das Original (`0x2367c`, `0x2390c`), die Gang-Lampe als Sprite; Drehzahlbild,
  Tempo- und Gangziffern entfallen dort. Die Startrichtung ist die des nächsten Straßenpunkts im
  Kartenraster (`0x120c0`, Aufruf `0x13ea1`), nicht die Richtung der Spline-Griffe (bis dahin
  1,6° daneben).
- **Routine 224, Rauch, Schatten, Radar** (2026-10-29, aus der EXE): Der Füller des Spiels
  (`0xcd9d0`) macht aus jedem Pixel `SMG[Texel × 256 + Pixel]`, ohne Licht oder Tiefe; das ist
  die einzige SMG-Abfrage im Programm. Benutzt wird sie vom Rauch (nur der Spieler, `0x19818`,
  `0x19970`: acht Plätze, alle 0,05 s an den Hinterrädern, fährt mit dem Auto, rund 0,67 s,
  zum Bildschirm gedreht; jede Wolke seitlich um (x & 7) · (T & 63) und nach hinten
  um (z & 7) · (T & 63) versetzt, x und z die ganzzahlige Lage des Autos, die das Spiel bei jedem Schritt zur 0 hin abschneidet,
  T sein 100-Hz-Zähler), vom Schatten (Spieler `0x18cbc`: 20 Einheiten von der Kamera weg,
  Boden − 2; Gegner `0x1c83c`, `0x1cd34`; Klasse 4 sortiert 200 weiter hinten), auf Strecke 4
  von Scheinwerferkegeln und Laternenschein, auf den Strecken 3 und 6 von einem Hubschrauber;
  von keinem Glas. Der Rasterizer liest dafür das Pixel aus dem Framebuffer und schlägt es in
  einer 16-KB-Tabelle nach (16 M10K, `RS_SMG`); Texel 0 lässt das Pixel stehen (Zeile 0 tauscht
  nur Farben gleichen Werts, das spart beim Rauch 23–75 % der Pixel). Im Mischmodus gilt die
  Punktregel des Originals, sonst würden die gemeinsamen Kanten der Dreiecke eines Vielecks
  zweimal gemischt. Das Radar (`0x23a68`) ist ein 72 × 72-Fenster bei (4, 124), abgedunkelt
  über Zeile 63 der Tabelle (× 0,75, nächste Palettenfarbe, `0x25870`) als ein gemischtes
  Viereck nach der Szene, darauf die Wände (MMP-Feld 3) als Blitter-Linien in Farbe 255, mit
  der Nase nach oben um (40, 160) gedreht, Gegner als Sprite 5, der Spieler als Sprite 4 bei
  (38, 158), ein Pixel 32768 / 24 Einheiten (Voreinstellung des Spiels, `0x1bc3e`).
- **Der Ton: ein Mischer in Hardware** (2026-10-29, nach OpenScreamers Notes Abschnitt 17 und
  der EXE): Der Treiber des Spiels läuft mit 11025 Hz und nimmt das gespeicherte Sample ohne
  Glätten (Index += Tonhöhe 16.16). `audio.v` macht dasselbe mit 48 kHz am Ausgang: je Ausgabewert
  ein Durchgang durch 32 Stimmen (vier Takte je Stimme, ein Rechenweg, ein DSP), das Byte an der
  Stelle mal Lautstärke links und rechts, Summe auf 16 Bit begrenzt, dann die Stelle um Tonhöhe ×
  11025 / 48000 weiter (die Firmware rechnet das um). Die Register stehen in MLABs, je Stimme
  eine Zeile von 32 Bytes in einem M10K, geholt über den Zeilen-Port; Takt der Ausgabe wie in der
  Vorlage (MCLK 12,288 MHz), alles auf `clk_74a`. Das C-Modell `firmware/mixer.c` rechnet
  Wert für Wert dasselbe (`tools/sim.sh audio`, 60.000 Zufallswerte). Lautstärke: Zeile von
  `SFXSBASE.DTR` mal Hauptregler, im Spiel `0x6000` (`0x1bc2f`), hier seit 2026-10-30 `0x4000`
  (−3,5 dB), damit der Motor des Spielers und die drei der Gegner zusammen nie übersteuern
  (in der Simulation vorher Spitze 27.355 nach 1 s Startaufstellung). Zu hören sind der Motor (eine
  Stimme, Tonhöhe Drehzahl × 14 + 32768 bei jedem Schritt, `autogall` im Tunnel), das Rutschen
  mit der Bedingung des Rauchs (`0x17d04`, Start und Halt bei `0x180d0`), die Wand (nur ohne
  Crash, `0x16f0b`), der Crash (`dentone`), Requisiten (`hit_0`) und die Berührung mit einem
  Gegner (`crash`, höchstens 30-mal je Sekunde). Seit 2026-10-30 auch Geräusche mit einem Ort,
  gehört an der Kamera (`docs/soc.md`, „Ton“): die Motoren der drei nächsten Gegner (in der
  Startaufstellung heulen sie auf) und die Fahrzeuge der Strecke (Lastwagen, Busse, Jet; die
  vierte Spalte von `ANIMATIONS`), mit Doppler und als einzige im Stereobild. Seit 2026-10-30
  der Sprecher, nur Firmware (`speech.c`, `docs/soc.md`, „Ton“): 58 Aufnahmen aus
  `SPFXBASE/SPFXFULL.DTR`, 27 Ereignisse mit den Auslösern, Zeiten und der Warteschlange des
  Spiels, eine Stimme in der Mitte, Hauptregler `0x3000` (mit `0x4000` übersteuerte der Countdown
  über Motoren und Musik). Offen: ein Tiefpass wie der
  Ausgangsfilter der Soundkarten
  (OpenScreamer nimmt 3,2 kHz wie die Sound Blaster Pro); ob er fehlt, entscheidet das Hören
  auf dem Pocket.
- **Die CD-Musik** (2026-10-30, OpenScreamers Notes Abschnitt 17, MUSIC): Das Spiel spielt Lied
  n als CD-Track n + 1, n aus `CD_TRACKS` in `MENU.DTR` (GEN.DAT): Streckenbildschirm Track 3,
  Strecken 1–6 Track 6, 4, 5, 7, 8, 2; endet der Track, spielt das Laufwerk ihn neu. Der Core
  liest dazu GOGs Abbild der CD von der SD-Karte, unverändert: `SCREAM.DAT` (Cue-Sheet, Slot 21)
  sagt, wo die Tracks in `SCREAM.gog` (Slot 22, roh, 2352 Bytes je Sektor) beginnen. Im Mischer
  ein Strom 16-Bit-Stereo mit 44,1 kHz, linear auf 48 kHz interpoliert, aus einem Ring von
  256 KB im SDRAM (`docs/soc.md`, „Ton“); `firmware/music.c` füllt ihn beim Start ganz (224 KB,
  auf dem Pocket rund 170 ms) und dann einmal je Bild mit einem Lesebefehl von 32 KB, den die
  CPU nur startet: Den Rest erledigt der Pocket über die Bridge, während das Bild entsteht.
  Lautstärke `0x4000`: Im Spiel ist der CD-Hauptregler `0x6000` (0,75); wie bei den Effekten
  sind es hier zwei Drittel davon, so bleibt das Verhältnis von Musik und Effekten wie im
  Spiel, und die Summe bleibt in 16 Bit (mit `0x6000` übersteuerten Musik und die Motoren in der
  Startaufstellung, in der Simulation 267 Werte in einer Sekunde).
- **Bewegliche Teile der Strecke** (2026-10-28, aus der EXE: `0x8ad14`, `0x8acf0`, `0x8ac58`;
  das Format nach OpenScreamers Notes S2-5b, das für beide Spiele gilt): Ein Mesh kann
  mehrere Blöcke haben, und ein Block mit Positionssatz (+0x0C) wird von dort verschoben oder,
  wenn das Mesh Bilder hat (+0x10), je Bild gedreht: 12 Wörter je Bild sind seine ganze Matrix
  (die gespeicherte Ecke als Zeile mal Matrix, 16383 = 1, dann der Versatz), das Bild ist
  Bilder × Uhr >> 16, eine Umdrehung der Animationsuhr in 3,33 s. Auf Strecke 1 hat nur der
  `H`-Satz so ein Mesh, `V220` am Start: den Sockel mit der Videowand (Block 0, ein festes
  Bild) und den Helm darauf (Block 1, 80 Bilder). Die Firmware zerlegt ein solches Teil in
  einen Auftrag je Block, den fernsten Mittelpunkt zuerst (`0x8ac58`). Die Bilder machen aus
  z oben y oben, eine Spiegelung; das Spiel nimmt für solche Blöcke die andere Seite als
  Rückseite (`0x8acf0`: `jge` statt `jl`), die Geometrie-Einheit ebenso (Bit 3 im Wort 18
  eines Auftrags, `docs/soc.md`). Das Objekt, das am Start rechts oben schwebte, war der
  Sockel ohne die Matrix seines Bildes, gekippt und zu hoch; den Helm gab es gar nicht.
- **Das Hintergrundbild wie im Original** (2026-10-27, aus der EXE): Das Rennen zeichnet
  vor der Szene `MAPn.BCK` (ein PCX, 512 × 256) mit `0x83a68`: eine reine Kopie, die Spalte
  `(2047 − Kurs) / 2 mod 512` am linken Rand (zweimal um das Bild je Umdrehung), die oberste
  Zeile bei Augenhöhe + 22 − 256 + 128 · sin(Nicken), 320 Byte je Zeile bei 512 Byte
  Zeilenabstand (eine Zeile, die über das Bildende reicht, liest in die nächste hinein);
  darunter bis zum Viewende die Farbe aus TRACK_DATA (`color`, vervierfacht bei `0x1337d`).
  Die geneigte Fassung `0x83b44` (vier texturierte Vierecke, um den Rollwinkel gedreht) ruft
  `STARTL.EXE` nirgends auf: Rollt die Kamera in einer Steilkurve, bleibt der Hintergrund
  aufrecht, so auch hier. Augenhöhe 130 (`0x4c610(320, 200, 130)`), die Mitte der Projektion
  in x ist Spalte 159 (`0x83431`), nicht 160. Der Blitter kopiert das Bild (deckend, ein Pixel
  pro Takt) und füllt den Rest (vier Pixel pro Takt), während die CPU die Szene auswählt;
  das erste Bauteil für die Geometrie-Einheit wartet, bis er fertig ist. Die CPU füllt den
  Himmel nicht mehr selbst.
- **Die Füllregel des Originals** (2026-10-27, aus der EXE): Die Füller laufen spaltenweise
  von der kleinsten bis zur größten Ecken-x, beide Spalten eingeschlossen (`0xda330`: jede
  Kante schiebt N + 1 Paare, das Paar am Eckpunkt wird überschrieben), und füllen jede Spalte
  vom abgeschnittenen oberen bis zum abgeschnittenen unteren Rand, beide gezeichnet
  (`0xda478`: `sar 16`, dann count + 1 Pixel). Ein Pixel (x, r) wird also gezeichnet, wenn das
  Polygon die Strecke von (x, r) nach (x, r + 1) berührt, das untere Ende ausgenommen. Das
  macht jedes Polygon ringsum etwa einen halben Pixel voller als die Regel „Pixelmitte im
  Dreieck“; Nachbarn überlappen an der Fuge, auch wo eine Ecke auf der Kante des anderen
  liegt (T-Stoß), und der Asphalt bleibt geschlossen. Mit der Mittelpunktregel und Ecken auf
  ganzen Pixeln blieben an den vier steilsten Schrägstellen von Strecke 1 61 Lochpixel (die
  Pocket zeigte sie in der Schrägkurve und unter der Brücke), mit der Regel des Originals 9,
  und die sind Spalten zwischen Teilen, die sich nicht berühren. Unser Rasterizer bleibt
  zeilenweise (der Framebuffer hält vier Pixel einer Zeile in einem Wort) und rechnet dieselbe
  Menge: je Zeile beide Kanten an Ober- und Unterkante, `docs/rasterizer.svg` Teil 1.
- **Bilder überlappen (Pipeline):** CPU, Grafik und Anzeige arbeiten gleichzeitig an Bild
  N+1, N und N−1, die Zeit pro Bild ist das Maximum statt der Summe. Kosten: ein Bild mehr
  Verzögerung zwischen Eingabe und Anzeige (20 ms bei 50 fps).
- **Zwei Framebuffer im Block-RAM** statt im PSRAM: Der Rasterizer schreibt den hinteren
  mit einem Pixel pro Takt, Übermalen kostet nichts, und es braucht weder PSRAM-Controller
  noch Arbitrierung für den Framebuffer (spart Logik, die knappe Ressource). Jeder Puffer
  hat seine eigene Palette, Palettenwechsel (Überblendungen) passen damit genau zum Bild.
  Die Zeichenseite liest auch (Rauch und Schatten verrechnen den Bildinhalt, Routine 224):
  vier explizite 8-Bit-Speicherbahnen mit Lese-/Schreibport (`fb_lane.v`), weil Quartus
  18.1 den Speicher mit echten Byte-Enables sonst stillschweigend aus Registern baut;
  `tools/check_budget.py` prüft bei jedem Build die 128 M10K. Getauscht wird am Anfang
  jeder Zeile der Austastlücke, höchstens einmal je Lücke (`video_out.v`): Ein Bild, das
  etwas länger als 20 ms braucht, fällt so nicht gleich auf 25 fps. Unabhängig
  gegengeprüft gegen SRAM, PSRAM und Mischformen (2026-10-25).
- **SRAM und PSRAM ausgebaut (2026-10-26):** Geplant war das SRAM für die Befehlslisten
  der Geometrie-Einheit und `cram0` für die Sounds. Die Geometrie-Einheit bekam stattdessen
  einen Puffer für 113 Bauteile im Block-RAM, und die 1,9 MB Sounds passen in den SDRAM.
  Beide Controller dienten nur noch dem Speichertest; sie zu entfernen spart wenig Logik
  (etwa 100 ALMs), aber Prüfaufwand und Fehlerquellen. Hinweise für einen Wiedereinbau
  stehen in `docs/speicher.md`.
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

## Reserven für später (etwa Screamer 2)

Gesammelt 2026-10-28, nichts davon gebaut. Screamer 2 zeichnet weiter (Tiefe 50.000, Strecke
4 60.000, gegen 43.808), hat Nebel und Abdunkeln je Fläche über eine 64-KB-Tabelle, Routine 208
für Schatten und Rauch und Spiegelungen auf den Autos, aber nur 4 Autos im Rennen
(OpenScreamer `docs/NOTES2.md`).

- **Bilder überlappen:** CPU an Bild N+1, Grafik an N; die Zeit je Bild ist das Maximum statt
  der Summe (am Start heute zusammen 82 %). Wenig Logik, ein doppelter Puffer für die
  Bauteile. Der größte Hebel.
- **Höherer Takt:** Der SDRAM kann mit CL2 bis 83 MHz (heute 74,25 MHz, bis 12 % mehr für
  alles); ein leererer Chip hält das Timing leichter.
- **Mehr Hardware an der Engstelle:** eine zweite Geometrie-Einheit, oder das Auswählen und
  Sortieren der Bauteile in Hardware (die CPU ist am Start der größte Posten).
- **Weniger Warten auf Texturen:** die Zeilen des nächsten Dreiecks schon anfordern, während
  das aktuelle gezeichnet wird (der Rasterizer wartet am Start rund 200.000 Takte je Bild,
  fast nur auf erste Zugriffe). Nicht über den SRAM: 55 ns asynchron sind rund 5 Takte je
  Zugriff (der Rasterizer braucht ein Texel pro Takt), und 256 KB fassen weder die 2,3–2,9 MB
  Texturen einer Strecke noch die rund 360 KB, die ein Bild berührt.
- **Nebeltabelle von Screamer 2:** je Fläche die eine benutzte Zeile (256 Byte) aus dem
  SDRAM holen statt der ganzen Tabelle im Block-RAM (sie bräuchte rund 52 M10K).
- **Block-RAM frei machen:** Boot-RAM von 32 auf 24 KB (8 M10K, ohne den Lader zu ändern;
  16 KB mit einem schlankeren Lader), eine gemeinsame Kehrwerttabelle für Setup-Einheit und
  Clipper (2 M10K), den angezeigten Framebuffer in den unbenutzten PSRAM (64 M10K; gezeichnet
  wird weiter im Block-RAM, nur das fertige Bild wird kopiert, ein Umbau mit Timing-Risiko).
  Textur-Cache und CPU-Caches nicht verkleinern (die Cache-Studie, die CPU ist der Engpass).

## Vorgehen

1. Grundgerüst: Template, zwei Framebuffer mit Palette, Eingabe (erledigt, auf dem Pocket
   getestet). Audio folgt ohne JTOPL: Das Spiel spielt in der GOG-Fassung die CD-Spuren
   (`SCREAM.gog`), nicht die MIDI-Musik (ein Mischer für Effekte, Sprecher und Musik).
2. VexRiscv mit FPU und SDRAM. Das erste Programm ist ein Speichertest: SDRAM, SRAM und
   PSRAM mit Mustern beschreiben, zurücklesen, Ergebnis auf dem Bildschirm. Damit werden
   die externen Speicher geprüft, bevor etwas auf sie baut (das SRAM erst hier, nicht
   vorher einzeln). Danach die Spiellogik von OpenScreamer bare-metal, erst mit
   Software-Renderer.
3. Bitgenaue C-Modelle von Rasterizer und Geometrie-Einheit, dann die Hardware dazu,
   jeweils in Verilator gegen das Modell geprüft.

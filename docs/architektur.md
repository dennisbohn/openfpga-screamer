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
steht aus: alle sechs Strecken, gewählt im Streckenbildschirm des Originals, danach das Auto
im Autobildschirm (siehe „Strecken- und Autowahl“), die Pause und „PLAY AGAIN?“, die
CD-Musik aus GOGs Abbild der CD, in den Menüs und im Rennen wie im Spiel, und die
Überblendungen und Menügeräusche des Originals; dazu Hauptmenü und Optionen (siehe
„Hauptmenü und Optionen“).
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
Strecke 1, geladen erst, wenn die Strecke gewählt ist) und für Strecken- und Autowahl
`GFX/MENU.DAT` (Slot 19) und `GFX/GRAF.DAT` (Slot 20). Von diesen beiden liest der Core nur
das Inhaltsverzeichnis und die Einträge, die er braucht, jeden an seiner Stelle wie in der
Datei (`load_parts` in `main.c`): 2,4 MB statt 5,9 MB, bei den gemessenen 1,3 MB/s des
Pocket rund 1,8 statt viereinhalb Sekunden beim Start (davon 1,4 MB `MENU.MLB`, ganz, weil
die Texturen der Autos über die ganze Datei verteilt liegen; ohne Autowahl waren es 0,7 MB;
seit 2026-10-30 dazu die sechs Ladebilder `LOADnL.PCX`, 0,28 MB, rund 0,2 s, und die
acht Bilder der Meisterschaft `CHAMPnL.PCX`, 0,33 MB, rund 0,25 s). `GAME.DAT`,
`GEN.DAT`, `LOCAL.DAT` und `SOUND.DAT` lädt er einmal, `SOUND.DAT` (1,9 MB, 1,3 s) während
die erste Strecke gebaut wird: die Firmware stößt das Lesen nur an (`sound_start` in
`main.c`), der Pocket schreibt die Datei in den SDRAM, während die CPU baut. Dazu zwei
nichtflüchtige Slots für die Spielstände, `CHOICE.DAT` (23) und `ATTACK.DAT` (24), im
Speicherordner des Pocket, nicht bei den Spieldateien (siehe „Meisterschaft“).
Dazu seit 2026-10-30 GOGs Abbild der CD oben im Spielordner: `SCREAM.DAT` (Slot 21, das
Cue-Sheet) und `SCREAM.gog` (Slot 22, 383 MB); daraus liest der Core nur die Musik, 32 KB auf
einmal, während er zeichnet (siehe „Die CD-Musik“).

- `DATA/`: alle 16 (`GEN`, `LOCAL`, `MUSIC`, `SOUND`, `MAP1`–`MAP6`, `MAP1I`–`MAP6I`)
- `GFX/`: `GAME`, `GRAF`, `MENU`, `MAP1L`–`MAP6L`, `MAP1H`–`MAP6H` (L und H sind die Sätze der
  Option DETAIL: L bei LOW, H ab MID; nicht die Auflösung)
- `TABS/`: `PERSP01.TAB`, `VIS.TAB`

## Hauptmenü und Optionen

Wie im Original (sein Bildschirm 0, die Seiten: Tabellen aus 16-Byte-Einträgen `[Zeile, id,
Wert, Höchstwert]` hinter `0x2de4`; OpenScreamer `docs/NOTES.md` Abschnitte 18 und 26), in
`firmware/menu.c`. Seite 0: NORMAL GAME, CHAMPIONSHIP, SPECIAL MODES, NETWORK GAME, OPTIONS;
Seite 1: DETAIL (LOW/MID/HI, anfangs HI), Schwierigkeit (ROOKIE/NORMAL/PRO, anfangs NORMAL),
LAP NUMBER (3/5/10/25, anfangs 3), SPEECH und MUSIC (anfangs ON), VIEW RECORDS; die Wörter
aus `MENU.TXT` (Zeilen 0–30; eine Option zeigt ihre Zeile + ihren Wert).

**Die Wörter sind Würfel im Menü-Szenenbild, wie im Original** (`0x2b33c`): jeder Buchstabe
ein Würfel `ALETTER.3DX` aus `MENU.DAT` (516 Einheiten), seine Textur (Material 1 von
`MENU.MLB`) hat 76 Bilder, eines je Zeichen; der Satz des Würfels hat bei +0x20
`0x35e·(Zeichen − 32) + 0x226`, ein Viertel davon ist die Textur-Phase der Geometrie-Einheit.
Vorn die helle obere Hälfte des Bildes, hinten die dunkle untere. Zeile i von n liegt auf
Reihe `8 − n + 2i`, y = `0x820 − 0x180·Reihe`, die Buchstaben 640 auseinander, zentriert
(`0x2af30`), auch die Leerzeichen; die Kamera steht bei (0, 0, 4100) und schaut nach −z,
Linse wie im Rennen (`0x29ea0`). Je Schritt (`0x2b8b8`) dreht sich die gewählte Zeile mit 4π
rad/s nach vorn, die übrigen nach hinten; jeder Würfel wogt in z um
`150·sin(2π·fx + π·fy + t·k)` (t mit 3π/s, bei 8π zurück; gewählte Zeile fx = 0,5, k = 1/4).
Beim Betreten fliegen die Buchstaben in einer Sekunde ein, Start und Schwung aus einer der
acht Zeilen von `0x2218`, reihum, der Start je Buchstabe um 0,09 rad gedreht. Seitenwechsel
oder geänderte Option: alle drehen sich weg, ist die gewählte Zeile über die Vierteldrehung,
steht die neue Seite da und ihre Zeile dreht sich fertig (`0x2b380`). Die Würfel gehen wie
die Modelle der Strecken- und Autowahl durch die Geometrie-Einheit, der fernste zuerst. Davor
`MENU1L.PCX` mit seiner Palette, darüber die beiden SCREAMER-Logos (Sprites 15 und 14 aus
`MENUL.SPR`, bei y 0 und 16), die von links mit 350 und 700 Pixel/s an ihren Platz gleiten
(`0x2df38`).

**Tasten** (`0x28080`, `0x28ac4`): hoch/runter von Zeile zu Zeile (ohne Umlauf), links/rechts
ändert eine Option, B oder START auch (das ENTER des Originals zählt hoch, `0x297d4`; erst wenn
alle Buchstaben gelandet sind). B oder START auf OPTIONS öffnet die Optionen, A oder SELECT
führt dort zurück. NORMAL GAME: die Kamera fährt zurück nach z 20.000 (Faktor 1 → 0 mit 0,6/s,
vierte Potenz) und rollt von −4π an mit π/s, die Buchstaben stehen, das Bild blendet mit 2/s
aus (`0x293e8`), dann die Streckenwahl; das Original fragt vorher den Namen ab (Bildschirm 4,
fehlt noch). NETWORK GAME zeigt wie das Original ohne Netzwerk `NETWORK NOT FOUND` (Zeile 55,
Schrift 0, Farbe 2, y 190, blinkend), SPECIAL MODES siehe „Spezialmodi“, CHAMPIONSHIP siehe
„Meisterschaft“, VIEW RECORDS spielt vorerst nur sein Geräusch. Die Seiten blenden mit 0,6/s
ein (`0x29ea0`): nach dem Start, nach A oder SELECT in der Streckenwahl und nach jedem Rennen
(`0x2a114` → `0x29ea0`).

**Die Optionen wirken** (gespeichert in `CHOICE.DAT`, siehe „Meisterschaft“): LAP NUMBER ist
die Rundenzahl des Rennens (`car_laps` in `car.c`; das Original nimmt `0x21e0` + 1: 3, 5, 10,
25), Ende des Rennens und „1/3“ der Anzeige folgen ihr. Die Schwierigkeit wählt die Datei
`MAPnA.DTR`, `MAPnB.DTR` oder `MAPnC.DTR` (`0x13251`; `track_build_difficulty`), aus der der
Bau der Strecke Gegner und `TRACK_DATA` liest. SPEECH OFF: der Sprecher sagt nichts
(`speech_on`), MUSIC OFF: keine CD-Musik (`music_on`), anders als im Original sofort, auch im
Menü (das Original übernimmt beides erst beim Laden des nächsten Rennens, `0x2a264`). DETAIL
wird nur angezeigt: der Core zeichnet immer den H-Satz ganz.

## Spezialmodi

SPECIAL MODES öffnet Seite 4 des Originals (`0x28eab`): TIME ATTACK, CONE CARNAGE, SLALOM
(`MENU.TXT` Zeilen 28–30, ids 28–30), A oder SELECT führt zurück. Die Wahl setzt das
Modus-Wort des Originals (`0x1d25c`: 1 normal, 2 Meisterschaft, 4 Zeitfahren, `0x10` Netzwerk,
`0x20` Cone Carnage, `0x40` Slalom; bei uns `car_mode` in `car.c`, aus `menu_mode()`), dann
wie NORMAL GAME Strecken- und Autowahl (`0x28bb0`; das Original fragt vorher den Namen ab).
Seite 3 (NEW/RESUME, ids 24/25) gehört dem Zeitfahren, Seite 5 (ids 26/27) der Meisterschaft.

In allen drei Modi (Maske `0x64`): allein auf der Strecke (`0x1311c`, `0x1309c`, `0x12e14`
legen keine Gegner an), eine Runde (`0x2a216`), vom Startplatz des letzten Gegners (`0x13ded`:
Spline Gegnerzahl − 1 statt des eigenen, Richtung die der Straße dort), keine Rundenbestzeit zu
schlagen (`0x2a264`: 0); nach dem Ziel keine Wiederholung (nur im normalen Spiel, `0x1fe45`),
gleich PLAY AGAIN?. Der Sprecher sagt dort kein „lead“ (Maske `0x13`, `0x24308`), in Cone
Carnage und Slalom auch kein „hurry“ (Maske `0x17`, `0x242dc`). Rekorde trägt das Original nur
für normales Spiel und Meisterschaft ein (`0x2e5e4`).

**CONE CARNAGE** (`0x20`): die Hindernisse aus `MAPnCNG.SPL` statt `MAPnOST.SPL` (`0x14fb4`;
Strecke 1: 36 Hütchen, Strecke 5: 99). Ziellinie und Checkpoints geben statt der Bonuszeit die
vierte Zahl von `TRACK_DATA`, `carnage` (`0x195fc`; Strecke 1 NORMAL 15 s statt 40), viel zu
wenig für eine Runde: jeder Schritt, in dem mehr Hütchen umgefahren sind als zuvor, gibt 1 s
(`0x18b50`; jedes zählt einmal, `+0x3c`, `0x126a4`). Im Ziel `YOU MADE IT!` (`GAME.TXT` 14,
y 90), mit allen Hütchen dazu `PERFECT!` (15, y 120; `0x23fbc`, `0x24204`), sonst wie immer
`TIME OVER`. Der Sprecher: „cones“ (Ereignis 19) einmal nach der Linie, „all“ (26) sobald alle
umgefahren sind (`0x24570`, `0x24386`), im Ziel „win“. `MODE=0x20` für `tools/render.sh`
zeichnet und fährt (`REPLAYCHECK`) den Modus auf dem PC.

**SLALOM** (`0x40`): die Hindernisse aus `MAPnSLA.SPL` (Torstangen, je zwei eine Straßenbreite
auseinander), die Tore aus `MAPnSLA.MRK` (Anzahl, dann 32 Byte je Tor: Radius 1024, `+0x10`
Sekunden 2–4, `+0x14` x, `+0x1c` y, die Mitte zwischen zwei Stangen; Strecke 1: 14 Tore).
Ziellinie und Checkpoints geben wie in Cone Carnage nur `carnage`. Je Schritt (`0x18b98`) das
erste Tor, in dessen Kreis der Wagen steht (seine ganze Stelle): beim ersten Mal gibt es seine
Sekunden und das Türgeräusch (Sample 9, `door`). Im Ziel `YOU MADE IT!`, `PERFECT!` nur mit
allen Toren und ohne eine Stange zu berühren (`0x23f68`, `0x241ac`); „all“ unter derselben
Bedingung. `MODE=0x40` für `tools/render.sh`.

**TIME ATTACK** (`4`, `firmware/ghost.c`): TIME ATTACK öffnet Seite 3 (`0x28e0d`), A oder
SELECT führt zurück zu Seite 4. NEW fährt allein (`0x28c0f`: `0x1d260` = 0), RESUME gegen den
Geist der Strecke (`0x28c3c`; ohne einen allein, `0x2a1df`). Normale Bonuszeit, „hurry“ ja.
Das Original zeichnet die Fahrt auf (`0x231d0`, eine Stellung je Bild) und spielt den Geist als
zweiten Wagen ab, mit dem nichts zusammenstößt (`0x12e14` → `0x1ba3c`, `0x14d28`; `0x23018`,
`0x23308`): das aufgezeichnete Auto auf dem Startplatz, von dem es fuhr, der Spieler auf dem
anderen (`0x13ded`: Aufzeichnung 0 oder 1 + Gegnerzahl − 1, `0x1bb33` wechselt), seine Runde
ist die zu schlagende (`0x1d268`, die dritte Uhr; darunter „new lap record“), der Spieler ist
Zweiter, solange der Geist weiter ist (`0x20418`). Im Ziel (`0x23ef0`): langsamer oder gleich
schnell `TRY AGAIN` (`GAME.TXT` 13) und „lose“, sonst (oder ohne Geist) `YOU MADE IT!` und
„win“; dann wird die Fahrt nach dem Rennen zum neuen Geist (`0x1fd64` → `0x1b964`,
`REPLAYS\MAPn.ATK`) und PLAY AGAIN fährt gegen ihn. Bei uns: eine Stellung je 4 Schritte
(12,5/s) ab dem Aufbau, abgespielt im selben Schritt, der Ort über die Kubik des Originals
durch vier Stellungen (`0x22724`), die Winkel zwischen zwei Stellungen gedreht; ohne Schatten
und Motor. Je Strecke der beste Geist, gespeichert in `ATTACK.DAT` (`ghost_track()` liegt
im Speicher dieses Slots, siehe „Meisterschaft“), im Format der Speicherdatei: 16 Byte Kopf „ATK1“, Strecke, Auto, Anzahl, Runde in 1/50 s, Startplatz; dann je
Stellung 20 Byte: x, y, Höhe als int32, Richtung, Neigung, Rollen als int16 mit 65536 = 2π, Stand
im Rennen × 64 und die Bremslicht-Bits; höchstens 3000 Stellungen, 60.016 Byte). `MODE=4
GHOST=1 REPLAYCHECK=9000 tools/render.sh` fährt zwei Roboterrennen, das zweite gegen den Geist
des ersten (Strecke 1: der Geist bis auf eine Einheit am Ort der ersten Fahrt).

## Meisterschaft

Wie im Original (sein Modus 2; OpenScreamer `docs/NOTES.md` Abschnitt 21, im EXE
nachgelesen): Regeln in `firmware/champ.c`, der Bildschirm 5 in `menu.c`, der Ablauf in
`main.c` (`championship`).

- **NEW / RESUME:** CHAMPIONSHIP öffnet die Seite 5 des Originals (`MENU.TXT` Zeilen 26/27,
  `0x28b3a`, A oder SELECT zurück). Beide verlassen das Menü wie NORMAL GAME. NEW (`0x28c6b`,
  `0x2c698`): Stufe 0, alle Plätze leer; RESUME (`0x28c9e`): zurück zur gemerkten Stufe.
- **Ablauf:** eine Tabelle von 53 Stufen bei `0x22b8` (je 44 Byte: Art, zwei Werte, Textblock,
  nächste Stufe, Stufe bei Misserfolg, Bild `champN`, Rennen, Schrift), vier Ligen (ROOKIE,
  AMATEUR, PRO, BULLET) zu 13 Stufen: vor jedem der sechs Rennen ein Textbild (WELCOME TO …
  vor dem ersten, sonst PREPARE FOR THE NEXT RACE!), nach jedem der Stand, nach dem sechsten
  CONGRATULATIONS!; Stufe 51 alles gewonnen, 52 LOSER!!. `champ_at` baut sie nach (regelmäßig,
  gegen den Dump geprüft).
- **Auto:** ENTER auf dem Textbild öffnet die Autowahl (`0x291ae`), vor *jedem* Rennen, mit
  dem zuletzt gewählten Auto; A oder SELECT dort führt zurück zum Textbild (`0x28118`). Fest
  für die Saison ist das Auto im Original nicht (OpenScreamer wählt es einmal bei NEW).
- **Rennen:** die Strecken 1–6 der Reihe nach, 3 Runden, die Gegner aus der Datei der Liga
  `MAPnA`–`MAPnD.DTR` (`0x28ef6`: `0x2a264(2, Strecke, Liga)`); in der Bullet-Liga (D: ein
  Gegner) fährt er die Bullet (`0x14373`: Schwierigkeit über PRO → Wagen 12, in der Lackierung
  seiner Reihe; `opp_bullet`). Ein beendetes Rennen geht nach den 5 s des Endbilds von selbst
  weiter, ohne Wiederholung und ohne PLAY AGAIN? (`0x1fd60`, `0x1fe86`); Zeit um oder
  verlassen (SELECT): PLAY AGAIN?, YES fährt es noch einmal.
- **Ergebnis** (`0x29acc`): ein verlassenes Rennen zählt nicht (die Stufe bleibt); sonst
  kommen Platz, Gesamtzeit und Punkte (9/7/5 für Platz 1/2/3, `0x21ec`) in seine Zeile, und
  die Stufe geht auf den Stand. Zeit um merkt sich 1, unerreichbar 2: wenn selbst Siege in
  den übrigen Rennen die Punktzahl der Liga (42/46/50/54) nicht mehr bringen.
- **Stand** (`0x2ce58`): `CHAMPnL.PCX` (n die Liga), der Kasten (16, 36, 288 × 136) zweimal
  abgedunkelt (`0x25cc8` mit der Tabelle für die Palette des Bildes, 0,75, `0x25870`); der
  Name der Liga (Schrift 2, Farbe 2, y 2), TRACK/POS/TIME/SCORE (y 48, x 28/88/170/246,
  Schrift 0, Farbe 15), je Rennen eine Zeile, 14 hoch ab y 62: `-n-`, Platz, `mm.ss.hh`,
  Punkte, für ein noch nicht gefahrenes Striche, für eins mit Zeit um `10`, `00.00.00`, `0`;
  4 tiefer TOTAL SCORE (rechtsbündig bis x 276), 24 tiefer QUALIFY:n zentriert. ENTER
  (`0x292c8`): weiter nur unter den ersten drei (Bullet-Liga: als Sieger), in der Zeit und
  nach dem sechsten Rennen mit der Punktzahl der Liga; sonst LOSER!!, und RESUME fährt das
  verlorene Rennen noch einmal.
- **Textbilder:** die Zeilen des Blocks aus `CHAMP.TXT` (Blöcke durch Zeilen mit `#`
  getrennt) zentriert ab y 32, eine Schrifthöhe auseinander, Schrift 3 (Liga gewonnen und Ende:
  2), Farbe 2. Auf allen Bildern blinkt PRESS ENTER TO CONTINUE (Zeile 56, y 190). B oder
  START ist ENTER, 10 s ohne Taste auch (`0x2d301`); A oder SELECT nur auf dem Textbild vor
  einem Rennen, zurück ins Hauptmenü (`0x2829d`). Ohne Geräusch; Einblenden mit 1,5/s,
  Verlassen mit Schnitt nach Schwarz wie die Autowahl.
- **Nach dem letzten Rennen:** Liga gewonnen → CONGRATULATIONS! → WELCOME TO der nächsten
  (das Original gibt dabei eine Strecke mehr frei, `0x290a5`; der Core bietet ohnehin alle
  an); nach der Bullet-Liga YOU'RE THE GREATEST DRIVER EVER … NOW THE BULLET IS YOURS !
  (`0x29141`: Bullet, gespiegelte Strecken und AFTERBURNER frei) und zurück ins Hauptmenü,
  ebenso nach LOSER!!.
- **Der Stand** liegt im Format der Einstellungsdatei des Originals, `CONFIG\CHOICE.DAT`
  (`struct choice_dat` in `champ.h`: 0x460 Byte, gelesen nach `0x203c0` von `0x1bbb8`, geschrieben
  von `0x1be5c` vor und nach jedem Rennen und beim Beenden): Auto, Strecke, Name, Optionen, ab
  +0x18 die Meisterschaft (Stufe, Stufe für RESUME, Punktzahl, je Rennen Zeit, Platz, Punkte),
  ab +0x6c die Rekorde (je Strecke fünf beste Runden und Gesamtzeiten), ab +0x42c
  Freischaltungen. Der Core hält genau diese Datei als nichtflüchtigen Data-Slot
  `CHOICE.DAT` (id 23) im SDRAM: beim Start geladen, ohne Datei die Vorgaben des Originals
  (`0x1bbb8`); jede Änderung (Stufe, Ergebnis, Optionen, zuletzt gewählte Strecke und Auto)
  schreibt die CPU gleich dorthin, und der Pocket speichert sie beim Beenden wie
  `debug.txt` (`docs/soc.md`, „Protokoll auf die SD-Karte“). Eine `CHOICE.DAT` aus dem
  DOS-Spiel geht auch. Die Rekorde stehen schon darin, für VIEW RECORDS später. Für das
  Zeitfahren (`REPLAYS\MAPn.ATK` des Originals: 36 Byte Kopf mit der Bestzeit bei +0x1c,
  dann je gezeichnetem Bild ein Satz von 40 Byte mit Ort, Lage und Uhr, `0x1b964`,
  `0x1ba3c`, `0x231d0`) hält der Slot `ATTACK.DAT` (id 24) je Strecke den behaltenen Lauf
  (`struct ghost_file`, 60.016 Byte, zusammen 360.096): `ghost_track(n)` zeigt direkt in
  seinen Speicher, so überlebt der Geist das Ausschalten wie `CHOICE.DAT`.

## Strecken- und Autowahl

Wie im Original (sein Bildschirm 3: `0x2e384`, `0x2c078`, `0x2cb88`, `0x29530`; OpenScreamer
`docs/NOTES.md` Abschnitte 18 und 26), einfachste Fassung in `firmware/menu.c`: Die sechs
Modelle `AMAPn.3DX` aus `MENU.DAT` stehen in einer Reihe, 40.000 Einheiten auseinander, und
drehen sich (π/4 rad/s); die Kamera schaut aus (−40.000·n, 4800, 13.000) 36° hinab und
gleitet bei links/rechts zum Nachbarn (Faktor 1 → 0 mit 0,95/s, Weg mit der vierten Potenz,
der nächste Druck zählt erst unter 0,8). Die Modelle gehen durch die Geometrie-Einheit wie
Objekte im Rennen, mit `MENU.MLB` als Materialdatei (alle ihre Flächen sind einfarbig,
Routine 128); jeder Block an seinem Platz aus dem
Positionssatz, der fernste zuerst. Davor das Bild `MENU3L.PCX` aus `GRAF.DAT` (seine Palette
ist die des Bildschirms), darüber Titel (`MENU.TXT` Zeile 45, Schrift 3), Streckenname
(`TRACKDAT.TXT`, Schrift 2, 2 Schrifthöhen über dem unteren Rand) und die Pfeile (Sprites 0
und 1 bei x 8 und 238, y 100, die 2 und 3 leuchten, solange die Kamera in ihre Richtung
gleitet und ihr Faktor über 0,6 liegt, `0x2e2f0`) aus `MENUL.FNT` und `MENUL.SPR`, alles über
den Blitter. B oder START nimmt die Strecke.

Danach, wie im Original („track selection, then car selection“), der Autobildschirm (sein
Bildschirm 2: `0x2bdcc`, `0x2ca10`, `0x2e08c`): für jede Zeile von `CARS.DTR` (GEN.DAT) ein
Auto, sein Modell `AAUTOnn.3DX` aus `MENU.DAT` in seiner Lackierung (Textur-Phase `0x6000`:
nicht bremsend, langsam), 4000 Einheiten auseinander bei z 2900, sich drehend mit π/2 rad/s;
die Kamera aus (−4000·n, 500, 4100), 22,5° hinab, gleitet wie oben. Die Autos zeichnet
`car_mesh_draw` (`car.c`) wie das Rennen ein Auto, mit der Blockreihenfolge der Engine; ihre
Texturen liegen über ganz `MENU.MLB` verteilt, deshalb lädt der Core die Datei ganz. Davor
`MENU4L.PCX` mit seiner Palette, darüber `CAR SELECTION` (Zeile 44), Name (Schrift 0, 3
Schrifthöhen tief), Getriebe (Zeile 37/38, 5 Höhen über dem Rand), `MAX SPEED` mit „%d KMH“,
`GRIP` und `ACCELERATION` mit ihren Balken (Sprites 4–8, 9–13), alles aus `CARDAT.TXT`. Jedes
Auto gibt es zweimal, Automatik in der ersten und Schaltung in der zweiten Lackierung, wie
die Zeilen von `CARS.DTR`; angeboten sind alle 13 Zeilen, auch der Bullet, wie alle sechs
Strecken (im Original erst nach Freischalten). Die Seitenansichten 16–28 aus `MENUL.SPR`
zeichnet `0x2e08c` nicht. B oder START nimmt das Auto, A oder SELECT führt zurück zur
Streckenwahl. Die Zeile ist das Auto des Spielers (Modell, Fahrdaten, Getriebe, Armaturenbrett
im Cockpit), die Gegner nehmen die übrigen Zeilen der Reihe nach (`0x142e8`, wie bisher).
Danach lädt, entpackt und baut der Core die Strecke (einige Sekunden; Strecke 1 auf dem
Pocket: 1 s Laden, 3,2 s Bauen, 0,6 s Effekte, Kulisse und Armaturenbrett). So lange steht,
wie im Original (`0x2a264`, vor jedem Rennen außer nach PLAY AGAIN), das Ladebild:
`LOADnL.PCX` aus `GRAF.DAT` mit eigener Palette, darüber `LOADING` (`MENU.TXT` Zeile 47)
zentriert in Zeile 0x50, Schrift 3, Farbe 0 (`0x2e598`), sofort mit seiner Palette; ist alles
aufgebaut, blendet es in einer halben Sekunde aus (siehe „Überblendungen“ unten;
`menu_loading` in `menu.c`, `loading` in `main.c`; `MENU=3` für
`tools/render.sh`). Die Musik des Menüs hört auf, wenn es erscheint: Die CPU baut die Strecke
am Stück und liest dabei aus den Data-Slots, den Ring der Musik füllt in der Zeit niemand.

**Pause:** START im Rennen, solange es läuft. Das Original hat dafür den CD-Spieler (P,
`0x211d0` → `0x21328`): das letzte Bild zweimal abgedunkelt (`0x25cc8`, dieselbe Tabelle wie
das Radar, 0,75), das Rennen steht, darüber das Autoradio `CDFRONTL.PKC` aus `GAME.DAT`, die
Audio-Tracks der CD (2–8, Schrift 5, Farbe 9, ab (64, 78) im Abstand 13, der laufende
blinkt) und der Cursor auf PLAY (Sprite 7 bei (59, 97)). Effekte und Musik blenden mit
16384/s von 0x6000 aus (1,5 s), dann hält die CD an (`0x26458`). Bei uns genauso, nur ohne
die Funktionen der Tasten: die CPU dunkelt das gezeigte Bild einmal über die Tabelle ab und
legt es als Standbild in den SDRAM (`still` in `main.c`), der Blitter kopiert es jedes Bild
und legt das Radio darüber (`hud_cd`). START noch einmal: Lautstärken zurück, die Musik der
Strecke (von vorn, wenn sie schon stand), wie PLAY.

**Die Wiederholung** (2026-10-30, aus der EXE): 5 s nach dem Ende eines beendeten Rennens,
sobald der Kommentator alles gesagt hat, was er sich vorgenommen hatte (`0x21048` fragt
`0x24740`; bis dahin der Endbildschirm über der Streckenkamera, siehe unten), läuft das
Rennen noch einmal; nach Zeitablauf oder SELECT nicht (`0x1fe28`). Das Spiel zeichnet dafür
nicht die Autos auf, sondern je Bild seine Steuerung (`0x22f34`), baut das Rennen neu auf
(`0x1fd64`, `0x128dc`) und liest sie statt der Tastatur zurück (`0x22e74`); alles andere
rechnet es neu. Wir halten je Physikschritt (1/50 s) das Tastenwort fest, das `car_step`
bekommt (`car_keys`: B, Y, links, rechts gehalten, L und R nur im Schritt des Drückens; 2
Bytes, bis 20 Minuten), bauen Effekte, Geräusche, Kulisse, Auto, Gegner und Musik wie zum
Start neu auf und lassen das Rennen aus den Wörtern laufen, mit so vielen Schritten je Bild
wie im Rennen auch. In der Wiederholung (`0x22d98` setzt das Flag `0x118e68`): von Anfang an
die Streckenkamera (`0x12afe`), keine Absturzkamera (`0x1857a`), die Ansichtstaste wirkt nicht
(`0x19b53`); von der Rennanzeige nur ein „R“ bei (8, 170), Schrift 0, Farbe 6, solange Bit 7
des 100-Hz-Zählers gesetzt ist (`0x2346c`; Beschriftungen, Countdown, Runde, Platz, Uhren
und Radar entfallen, OpenScreamers Notizen irren hier), dazu Startampel (`0x20d10`) und
Endbildschirm (`0x21048`) wie im Rennen. Kommentar und Geräusche kommen wie im Rennen (das
Spiel fragt das Flag dort nicht ab); der Rundenrekord bleibt, wie ihn das Rennen hinterließ,
deshalb kein zweites „new lap record“. Sind die Wörter aufgebraucht, endet die Wiederholung
(`0x22dd8`), B, START oder SELECT überspringen sie (ENTER und ESC im Original); dann PLAY
AGAIN?. Geprüft auf dem PC (`REPLAYCHECK` für `tools/render.sh`): ein ganzes Rennen eines
Fahrroboters auf Strecke 1 (Auto 6, 8391 Schritte, drei Runden) und seine Wiederholung, in
anderen Bildtakten und auf der Streckenkamera, sind in jedem Schritt bitgleich (Spieler und
neun Gegner).

**PLAY AGAIN?:** SELECT im Rennen, nach Zeitablauf (5 s nach dem Ende, wie oben) und nach der
Wiederholung. Das Original fragt nach jedem
verlassenen Rennen (`0x1fe86` → `0x219d0`, außer in Meisterschaft und Netzwerk; nach einem
beendeten Rennen erst nach der Wiederholung): ESC stoppt die Effekte
(`0x1f8cc`), die Musik läuft weiter; `0x219d0` zeichnet die Szene selbst noch einmal (`0x81870`,
320 × 200, auch aus dem Cockpit) ohne Rennanzeige und Endbildschirm, also ohne Platz, Gesamtzeit,
TIME OVER und das „R“ der Wiederholung. Wir zeichnen dafür ein letztes Bild ohne Physikschritt
und ohne Rennanzeige, Radar und Armaturenbrett (`ask` in `run_track`), in allen Fällen gleich:
SELECT, Zeitablauf und nach der Wiederholung. Dieses Bild dreimal abgedunkelt, darüber `GAME.TXT`
Zeilen 19–21 (PLAY AGAIN?, YES, NO) zentriert in den Zeilen 52, 92 und 122 (Schrift 0), die
Frage in Farbe 2, die Antworten in 14, die gewählte blinkt nach 2. Hoch/links YES,
runter/rechts NO, jeder Druck mit dem Klick (siehe „Menügeräusche“), B oder START nimmt die
Antwort, A oder SELECT ist NO (ESC im Original). YES baut das Rennen neu auf (ohne die
Strecke neu zu bauen), NO führt ins Hauptmenü.

**Überblendungen** (2026-10-30, aus der EXE): alle über die Palette, jeder Wert mal f zu
Schwarz hin (`0x27de8`; bei uns `pal_faded` in `main.c`, in die Palette des Puffers, der als
nächster gezeigt wird). Wird ein Menübildschirm verlassen (Strecke → Auto, Auto → Strecke,
Auto → Rennen), ist es sofort schwarz (`0x1d220` = 0: die Palette schwarz im Bild des
Tastendrucks; `black`), der neue Bildschirm blendet mit 1,5/s ein, in 2/3 s (`0x20374` aus
`0x2c078` und `0x2bdcc`, `0x29738` in jedem Bild), Modelle und Kamera laufen dabei schon; ebenso
die Seiten des Hauptmenüs, mit 0,6/s (siehe „Hauptmenü und Optionen“). Einen Kameraflug gibt es zwischen Strecken- und
Autobildschirm nicht (nur beim Verlassen der Menüseiten). Das Ladebild erscheint ohne
Überblendung (`0x2a264`) und blendet, wenn das Rennen aufgebaut ist, in einer halben Sekunde
aus, nur das Bild (`0x128dc(1)` → `0x21d8c(0)`; OpenScreamers Notizen übersehen das). Das
Rennen blendet in einer halben Sekunde ein, Bild, Effekte und Musik (`0x21e50` in jedem Bild,
2/s); verlassen (PLAY AGAIN? NO, `0x1feed`) oder neu aufgebaut (YES, die Wiederholung:
`0x128dc(0)`) blendet es ebenso aus, mit dem Ton (`0x21d8c(1)`). Das Spiel steht beim
Ausblenden; wir lassen die Bildschleife laufen, damit die Musik weiter Daten bekommt: das
stehende Bild in beide Puffer kopiert, je Bild bekommt einer die Palette und wird getauscht
(`fade_out`). Die Pause blendet nicht.

**Menügeräusche** (2026-10-30, aus der EXE): Solange ein Menü da ist, ist die Sample-Tabelle
des Spiels `SOUND_FX` aus `MENU.DTR` (GEN.DAT: `smenu1`–`smenu9`, `MENU1.RAW`–`MENU9.RAW`;
`0x29e1d`, `0x1f2d0`). Im Strecken- und Autobildschirm spielt nur der Schritt zur nächsten
Strecke oder zum nächsten Auto etwas, Zeile 6, `smenu7` (`0x2859e`, `0x28617`, `0x2874b`,
`0x287cd`); Wählen und Zurück sind still (die Zweige `0x28ef6`, `0x28172` und `0x2821e` spielen
nichts; OpenScreamer spielt dort Zeile 0, nach eigener Lesart). Auf den Seiten: Zeile 1 von
Zeile zu Zeile (`0x283ca`, `0x284ba`), Zeile 2 für eine geänderte Option (`0x28df0`), Zeile 0
beim Öffnen einer Seite und bei ESC zurück (`0x28b5f`, `0x280ec`), Zeile 3 für NORMAL GAME
(`0x28ba2`); NETWORK GAME ist still. Die neun Samples liest der
Core beim Start an ihren Stellen aus `SOUND.DAT` (88 KB; der ganze Container folgt mit der
ersten Strecke), die Tabelle setzt er nach jedem Rennen neu (`snd_menu` in `sound.c`). PLAY
AGAIN? klickt bei jedem Druck von hoch/links oder runter/rechts (`SFXSBASE.DTR` Zeile 6,
`sclick`; `0x21b0e`, `0x21b47`). Der CD-Spieler der Pause spielt nichts.

Nicht dabei: Namenseingabe, Rekorde, Intro, die gespiegelten
Strecken, „NEW TRACK AVAILABLE“,
Wiederholungen als Datei (`REPLAYS\TEMP.RPL`). Die Simulation fährt gleich die Strecke aus `SIM_TRACK` (Slot 28) mit dem
ersten Auto; mit `SIM_MENU=1` (Slot 27) zeigt sie stattdessen den Streckenbildschirm, mit
`SIM_MENU=2` den Autobildschirm, und prüft das dritte Bild gegen die C-Modelle (2026-10-30:
beide 0 Pixel Unterschied; der Autobildschirm braucht 0,22 M Takte, 3 ms je Bild). Den
Abgleich des Rennbildes mit den C-Modellen (`track_check`) macht nur die Simulation: Auf dem
Pocket zeichnete die CPU das Bild dafür eine Sekunde lang nach und hielt das Rennen gleich nach
„three“ an; dort steht jetzt `track check: simulation only` im Protokoll.
`TRACK=n`, `CAR=r` und `MENU=1` oder `2` für `tools/render.sh` zeichnen Start mit Auto r,
Strecken- oder Autobildschirm auf dem PC, `MENU=4` oder `5` Hauptmenü oder Optionen
(`MENUKEYS="DDDDB"` drückt vorher Tasten), `PAUSE=1` oder `2` Pause oder Frage über dem Start.

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

Gemessen am 2026-10-30 (Quartus-Build mit dem Lesen über die Bridge aus dem SDRAM für die
Spielstände, Timing hält bei 74,25 MHz: Setup +0,78 ns, Hold +0,08 ns; Bauzeit 5,5 min): 10.902
von 18.480 ALMs (59 %), 282 von 308 M10K (92 %, davon 16 die Tabelle der Routine 224), 31 von
66 DSP (47 %). Das Lesen kostet rund 46 ALMs (`sdram_port` 102 → 118, die eigene Logik von
`soc` 554 → 585: Leseregister, Antwort, Größe in die Slot-Tabelle) und kein M10K; dass die
Summe sank, ist die Platzierung. Davor (mit den Spiegelungen der Routine 160): 11.046 ALMs,
Setup +0,85 ns, Hold +0,01 ns. Die Spiegelungen kosten 727 ALMs und 1 M10K in der Geometrie-Einheit (678 → 1.405 ALMs: die
Auswahl vor ihren drei Multiplizierern, jetzt 18 Bit breit, der Normierer des Eckpunkts, die
Umrechnung der Normale, Summen und Register der Rechnung, die Tabelle), keinen DSP. Davor
(mit der CD-Musik): 10.278 ALMs, 281 M10K, Setup +0,94 ns, Hold +0,06 ns. Der Mischer mit der Musik:
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

![Platzbudget im FPGA: Logik 59 %, Block-RAM 92 %, DSP 47 %](ressourcen.svg)

Die Grafik erzeugt `tools/make_budget_svg.py`: die gemessenen Zeilen aus dem Fitter-Report
(`core/src/fpga/output_files/ap_core.fit.rpt`, Tabelle „Resource Utilization by Entity“), die
geplanten aus Schätzungen im Skript. Nach jedem Build neu erzeugen; das Skript schreibt auch
diese Tabelle.

| Komponente | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue-Rahmen (Bridge, PLLs) | 468 | 2 | – |
| Framebuffer ×2 + Video | 124 | 130 | – |
| VexRiscv + 2× 8 KB Cache | 1.254 | 19 | 4 |
| FPU (RV32F) | 1.443 | 5 | 4 |
| CFU (16.16-Multiplikation) | 8 | – | 3 |
| Boot-RAM 32 KB + Protokoll | – | 36 | – |
| Geometrie-Einheit | 1.254 | 23 | 4 |
| Clipper | 956 | 6 | 2 |
| Setup-Einheit | 1.513 | 7 | 4 |
| Rasterizer | 1.235 | 2 | 5 |
| Texturen: Pipeline + Cache | 694 | 47 | 4 |
| Blitter (Hintergrund, HUD) | 481 | 2 | – |
| SDRAM-Controller + Ports | 316 | 2 | – |
| Audio: Mischer 32 Stimmen + Musik, I²S | 571 | 1 | 1 |
| Bus, Register, Arbiter | 584 | – | – |
| **gebaut** | **10.901 (59 %)** | **282 (92 %)** | **31 (47 %)** |

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
- **Die Federung** (2026-10-30, aus der EXE: `0x168a4`, gezeichnet von `0x15c0c`): Jedes Rad
  schwingt um bis zu 18 Einheiten. Die Tempoänderung des Bildes × 0,00175 stößt die Vorderräder
  in die eine, die Hinterräder dreifach in die andere Richtung; die Drehrate × Tempo² × 1,5e-9
  die linken gegen die rechten; beides durch die Bildzeit. Ein größerer Stoß startet die
  Schwingung neu, cos(4π t + π), sie klingt ab (Steifigkeit 61 quer, 71 längs, unter 2 aus).
  Aus den Ausschlägen wie aus Radhöhen: Nicken `+0x4e0` = atan((VR − HR) / Radstand), Rollen
  `+0x4e8` = −atan((VL − VR) / Spur) mit der negativen Spur des Spiels, Senken `+0x4c0` =
  Mittel. Nur die vier Karosserieblöcke (`MV_mauto`, `fauto`, `rauto`, `fai`) nicken um
  `+0x4e0` × 2 und rollen um `+0x4e8` × 4 und sinken um `+0x4c0`, die Räder bleiben auf der
  Straße: in der Kurve neigt sich der Aufbau nach außen, beim Bremsen taucht die Nase ein.
  Stoßstange und Cockpit (`0x1a2ac`) nicken um `+0x4e0`, rollen um 3 × `+0x4e8` und heben sich
  um `+0x4c0`. Weil der Querstoß zweimal durch die Bildzeit geht und die Schwelle 2 je Bild
  gilt, läuft die Federung (`suspension_step` in `car.c`) auf den Bildern des Spiels von
  1/30 s, die Steifigkeiten wie die Tabelle des Spiels bei 30 Bildern/s. Bei 140 km/h und voll
  eingeschlagen rollt die Karosserie rund 3°, beim Vollbremsen nickt sie rund 3°. Die Gegner
  (`0x1df24`, unten) rollen um v × 0,02 und nicken um −|v × 0,02|, die Nase also in jeder Kurve
  nach unten (vorher nickten sie um v × 0,02 wie sie rollen).
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
- **Die Streckenkamera am Rennende** (2026-10-30, aus der EXE, Zeile 3 der Tabelle,
  `0x1a620`): Ist das Rennen beendet (Flag 8 bei `0x18e14`) oder die Zeit um und das Auto
  steht (0x10), schaltet das Spiel auf sie um. Ihre Plätze sind die Knoten von `MAPnRPL.SPL`
  (der Lader `0x14fb4` legt die Datei bei `0xf90c` ab; wir lesen sie beim Rennaufbau aus
  `DATA/MAPn.DAT`, `car_trackside_init`). Genommen wird der Knoten, der dem Auto am Boden am
  nächsten liegt (ganze Einheiten, Abstand abgeschnitten, ein Knoten genau über dem Auto
  zählt nicht); von dort schaut sie geradewegs auf das Auto, um den Höhenwinkel hinab
  (`2π − acos(flach/Abstand)`, auch wenn der Knoten tiefer liegt), ohne Neigung. Ist das Auto
  weiter als 1500 weg, rückt sie auf ihrer Blicklinie heran, bis es 1500 sind (jede
  Koordinate abgeschnitten). Auto und Schatten werden gezeichnet; Overlay 0: von der
  Rennanzeige bleiben die Beschriftungen (`IMPLET_L`), Countdown, Runde, Platz, Uhren und
  Radar, Drehzahlbild, Tempo und Gang entfallen (`0x2367c`). Der Wechsel schneidet (wir haben
  noch keine Kameraflüge). `TRACKSIDE=1` (mit `HUD=1` samt Rennanzeige) für
  `tools/render.sh` zeigt sie nach den Schritten.
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
- **Spiegelungen auf den Autos, Routine 160** (2026-10-30, aus der EXE, am Speicher des
  Originals geprüft): Die Scheiben zeigen ein Spiegelbild, das mit Fahrt und Blick wandert.
  Das Original rechnet für jeden Eckpunkt eines Blocks mit Flag 0x2000 (die Karosserie, 84–128
  Eckpunkte mit Normale) auf dem x87 die Spiegelung r = 2 (e·n) n − e der Blickrichtung e an
  der Normale n (`0xc2230`) und nimmt u = round(127 r.x) + 127 − offx, v = round(127 r.z) +
  127 − offz als Texel, ohne Materialgröße, bei 256 herumlaufend. Bei uns die
  Geometrie-Einheit nach den Eckpunkten des Blocks, ganzzahlig (`firmware/geom.h`): die
  Normale aus den Float-Bits auf Q14, mit der Matrix in den Sichtraum (derselbe Schritt wie
  für die Eckpunkte), der Eckpunkt auf 17 Bit normiert, 1/|p| über eine Tabelle von 128 und
  einen Newton-Schritt auf ihren drei Multiplizierern (jetzt 18 × 18), rund 26 Takte je
  Eckpunkt; u, v in einem RAM von 512 (1 M10K), aus dem die Flächen der Routine 160 ihre
  Texel nehmen, der Rasterizer lässt sie bei 256 herumlaufen (Größe Bit 29). Gegen die
  Gleitkomma-Form ist rund 1 % der Werte um eins anders. Was das Original je Objekt rechnet,
  bleibt auf der CPU (`track.c`): die Versätze offx, offz aus drei Akkumulatoren je Objekt
  (`0xc1fc0`: der Weg seit dem letzten Zeichnen, in die Sicht gedreht, mal dem Faktor an
  `0xc2428`) und das Bild: `0x1813c` setzt für das Spielerauto nach der Art des zuletzt
  erreichten Markers Auto +0x7b0 = 0 / 0x8000 / 0xff00 (Art 0 / 1 / 2) und den Faktor 0,002
  bzw. 0,03, `0x1607c` kopiert es in den Objektsatz (+0x3c), gezeichnet wird Bild n × Phase
  >> 16. Im Tunnel von Strecke 1 (Marker 3 und 4, Art 1) ist das Bild 1 der Textur, die
  Lichter der Tunneldecke, statt des Himmels; die Gegner behalten Bild 0 (sie rufen `0x1813c`
  nicht auf). Gemessen am Original (DOSBox, Startaufstellung, nahe Verfolgerkamera;
  `tools/render.sh` mit `CAM` und den Akkumulatoren aus seinem Speicher, `REFL`): von den 128
  Eckpunkten der Karosserie 95 gleich, die übrigen um eins; in der Heckscheibe 65 von 125
  Pixeln gleich (84 % gleich einem Nachbarn), mit den gespeicherten u, v vorher 9. Kosten in
  der Simulation: Geometrie-Einheit +36.000 Takte, Startbild 1.338.092 Takte = 18,0 ms
  (vorher 17,6 ms; die Tabelle der sechs Startbilder oben ist nicht neu gemessen).
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

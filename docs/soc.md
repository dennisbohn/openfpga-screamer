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
  `0x00000000` ins Boot-RAM: der Lader (Selbsttests, kurzer Speichertest). Data-Slot 2
  `program.bin` an `0x1300_0000` ins SDRAM: das Programm.
- **Slots im SDRAM erst nach der Phasensuche:** Die Slots 2 (`program.bin`) und 3 bis 8 (die
  Spieldateien) haben `deferload`, der Pocket lädt sie nicht von selbst. Die Phasensuche des Speichertests
  stellt den SDRAM auch in schlechte Phasen, dort kann der Chip Befehle und Adressen falsch
  übernehmen und irgendwo schreiben (gemessen: 15 von 63 MB verändert). Der Lader fordert
  `program.bin` deshalb erst nach den SDRAM-Tests an (`DS_*`, der Ziel-Befehl 0x0180 von
  `core_bridge_cmd`), zuerst die 16 Bytes des Kopfes, in dem die Größe steht, dann die Datei;
  das Programm holt die Spieldateien genauso (`firmware/slot.c`). Deren Größe steht in der
  Slot-Tabelle, die der Pocket für jeden Slot über die Bridge schreibt (`0xF800_2000`, je
  Slot Nummer und Größe, auch bei `deferload`): `DS_SIZE` liest sie, denn das
  Inhaltsverzeichnis eines Containers liegt an seinem Ende. Die Bridge liefert 32-Bit-Wörter
  big-endian (das erste Byte der Datei in Bit 31:24), die CPU ist little-endian: `soc.v`
  dreht die Byte-Reihenfolge beim Schreiben um, beide sind ganz normale little-endian-Abbilder.
- **Start des Programms:** `program.bin` beginnt mit einem Kopf aus vier Wörtern: `"SCRP"`
  (`0x50524353`), Größe in Bytes, CRC-32 der Bytes nach dem Kopf, Einsprungadresse. Der Lader
  prüft ihn nach dem Speichertest (Test „program“, `SYS_SIM` Bit 4), leert den D-Cache, macht
  `fence.i` und springt mit `a0` = seine Ergebnisse (`SYS_SIM`-Bits), `a1`, `a2` = Position
  und angefangenes Wort im Protokoll. Das Programm setzt eigenes `gp`, `tp` und `mtvec`, sein
  Stack liegt oben im Boot-RAM (ab `0x8000` abwärts bis `0x6000`; der Lader bleibt darunter).
  Auf den Bildschirm kommt dabei nichts (ein Tausch mit schwarzer Palette, damit nach einem
  Reset nicht das Bild des vorigen Laufs stehen bleibt); das erste Bild ist das Ladebild des
  Programms. Schlägt ein Selbsttest oder ein Test des Speichertests fehl oder fehlt
  `program.bin`, startet der Lader nichts, und die Ergebnisse bleiben auf dem Bildschirm
  (seit 2026-10-31; davor zeigte er den Speichertest bei jedem Start, und Select beim Start
  prüfte den ganzen Speicher).

## Speicherkarte der CPU

| Adresse | Größe | Inhalt | Cache |
|---|---|---|---|
| `0x0000_0000` | 32 KB | Boot-RAM: der Lader, darüber der Stack des Programms. Reset-Vektor `0x0000_0000`, `mtvec` nach Reset `0x0000_0020` | ja |
| `0x1000_0000` | 64 MB | SDRAM: die Teile von `MENU.DAT` ab `0x1020_0000` und `GRAF.DAT` ab `0x1060_0000` (Slots 19, 20, jeder an seiner Stelle wie in der Datei) und Bilder, Sprites und Schriften der Menüseiten, der Strecken- und Autowahl und das entpackte Ladebild ab `0x10A0_0000`, das Standbild der Pause und von „PLAY AGAIN?“ ab `0x1011_0000`, die Front des CD-Spielers ab `0x1012_0000`, die Soundeffekte `SOUND.DAT` ab `0x11C0_0000` (Slot 8, bis 2 MB; der Mischer spielt sie von dort; die neun Samples der Menüs schon beim Start, jedes an seiner Stelle), der Ring der Musik ab `0x11E0_0000` (256 KB, aus `SCREAM.gog`, Slot 22), das Abbild der Strecke ab `0x1210_0000` (von der Firmware gebaut), die Spieldateien ab `0x1280_0000` (`MAPnH.DAT` `0x1280_0000` (Slot 3, 9–13), `GAME.DAT` `0x12C0_0000` (4), `MAPn.DAT` `0x12D0_0000` (5, 14–18), `GEN.DAT` `0x12D4_0000` (6), `LOCAL.DAT` `0x12D5_0000` (7)), Arbeitsspeicher des Aufbaus ab `0x12D6_0000`, das Programm ab `0x1300_0000` (Slot 2, höchstens 15 MB), die Spielstände `CHOICE.DAT` ab `0x13F0_0000` und `ATTACK.DAT` ab `0x13F1_0000` (Slots 23, 24, nichtflüchtig) | ja |
| `0xF000_0000` | 4 KB | System- und Speicher-Register | nein |
| `0xF000_1000` | 4 KB | Video-Register und Palette | nein |
| `0xF000_2000` | 4 KB | Eingabe | nein |
| `0xF000_3000` | 4 KB | Rasterizer, Setup- und Geometrie-Einheit, Blitter | nein |
| `0xF000_4000` | 4 KB | Ton: der Mischer | nein |
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
| `0xF000_002C` | `DS_LENGTH` | W | Bytes, ein Vielfaches von 4, oder `0xFFFF_FFFF` für den Rest der Datei (der Pocket bemisst ihn; so liest die Firmware die letzten 1 bis 3 Bytes) |
| `0xF000_0030` | `DS_CTRL` | R/W | Schreiben: Bit 0 = anfordern. Lesen: Bit 0 läuft noch, Bit 1 `DS_SIZE` wird noch gesucht, Bit 6..4 Ergebnis des Pocket (0 = gut, 2 z. B. keine Datei im Slot) |
| `0xF000_0034` | `DS_SIZE` | R/W | Lesen: die Dateigröße von Slot `DS_ID` aus der Slot-Tabelle des Pocket, 0 wenn er dort fehlt; nach jedem Schreiben von `DS_ID` neu gesucht. Schreiben: der Wert wird die Größe von Slot `DS_ID` in der Tabelle (die Spielstände, siehe unten; Bit 1 von `DS_CTRL`, bis er eingetragen ist) |
| `0xF000_1000` | `VID_STATUS` | R | Bit 0: vorderer Puffer (0/1). Bit 1: Tausch angefordert, noch nicht vollzogen. Bit 31..16: Bildzähler, zählt jedes Videobild (50 Hz) |
| `0xF000_1004` | `VID_SWAP` | W | beliebiger Wert: Tausch anfordern. Er geschieht in der nächsten Austastlücke; solange er aussteht (Bit 1), werden weitere Anforderungen ignoriert |
| `0xF000_1400` + 4·i | `VID_PAL[i]` | W | Palette des **hinteren** Puffers, Eintrag i (0..255): `0x00RRGGBB`. Nur solange kein Tausch aussteht (`VID_STATUS` Bit 1 = 0); sonst trifft es den Puffer, der gerade angezeigt wird |
| `0xF000_2000` | `IN_KEY` | R | Tasten (`cont1_key`): Bit 0 hoch, 1 runter, 2 links, 3 rechts, 4 A, 5 B, 6 X, 7 Y, 8 L, 9 R, 14 Select, 15 Start; Bit 31..28 Gerätetyp (1 Pocket, 2 Dock-Controller ohne, 3 mit Analogteil) |
| `0xF000_2004` | `IN_JOY` | R | Analogsticks (`cont1_joy`): Bit 7..0 links x, 15..8 links y, 23..16 rechts x, 31..24 rechts y; vorzeichenlos, Mitte `0x80`, 0 = links/oben. Nur bei Typ 3, sonst 0 |
| `0xF000_2008` | `IN_TRIG` | R | Analog-Trigger (`cont1_trig`): Bit 7..0 links, 15..8 rechts; 0 = losgelassen |
| `0xF000_3000`–`3008` | `RS_V0`–`RS_V2` | W | Rasterizer (`raster.v`, dieselben Pixel wie `firmware/raster.c`): Eckpunkt, Bit 15..0 x, Bit 31..16 y, vorzeichenbehaftet in 1/16 Pixel; darf geschrieben werden, während ein Dreieck gezeichnet wird |
| `0xF000_300C`, `3010` | `RS_CLIP0`, `RS_CLIP1` | W | Clip-Rechteck in Pixeln: x0/y0, x1/y1 (ausschließlich), Bit 15..0 x, Bit 31..16 y |
| `0xF000_3014` | `RS_DRAW` | R/W | Schreiben: Bit 7..0 Farbe, Bit 8 Puffer, startet das Dreieck; der Schreibzugriff wartet, solange der Rasterizer zeichnet, Framebuffer-Zugriffe der CPU ebenso. Lesen: Bit 0 zeichnet noch |
| `0xF000_3018` | `RS_PIXELS` | R/W | gezeichnete Pixel; Schreiben löscht |
| `0xF000_301C`–`304C` | `RS_TEX` … `RS_CTRL` | W | Texturen (`firmware/hw.h`): Byte-Adresse von Texel (0, 0) im SDRAM, Größe und Durchsichtigkeit, Bezugspixel, Q, U, V mit Änderung je Pixel; `RS_CTRL` leert den Textur-Cache; `RS_DRAW` Bit 9 zeichnet texturiert. In der Größe (`RS_TEXSIZE`, ebenso `SU_TEXSIZE` und das Vieleck der Geometrie-Einheit) Bit 31 `RS_CUT`: Texel 0 durchsichtig; Bit 29 `RS_WRAP`: Routine 160, u und v laufen bei 256 herum statt an der Größe zu enden (wie die Texeladresse `& 0xffff` des Originals); Bit 30 `RS_BLEND`: Routine 224 des Spiels, das Pixel wird `SMG[(Texel & 63) × 256 + altes Pixel]` (der Rasterizer liest es aus dem Framebuffer, ein gemischtes Pixel kostet bis 2 Takte; Abdeckung nach der Punktregel des Originals, damit gemeinsame Kanten eines Fächers nicht doppelt gemischt werden) |
| `0xF000_3050` | `RS_MISSES` | R/W | Textur-Zeilen aus dem SDRAM nachgeladen (Cache-Fehltreffer); Schreiben löscht |
| `0xF000_3054` | `RS_CLOCKS` | R/W | Takte, in denen der Rasterizer beschäftigt war; Schreiben löscht (auch die fünf folgenden) |
| `0xF000_3058`–`3068` | `RS_MISS_CLOCKS` … `RS_STARVED` | R | Takte, in denen der vorderste Pixel der Textur-Warteschlange auf seine Zeile aus dem SDRAM wartete; in der Kantenvorbereitung; im Aufwand je Zeile (Zeilen, Spanne, Werte am ersten Pixel); untätig, während das nächste Dreieck der Setup-Einheit auf das Textur-Fließband wartete; untätig, während Setup- oder Geometrie-Einheit arbeiteten |
| `0xF000_3080`, `3084` | `SU_CENTRE`, `SU_SCALE` | W | Setup-Einheit (`setup.v` mit `clipper.v`, bitgenau `firmware/setup.c`): Mitte der Ansicht in 1/16 Pixel (wie `RS_XY`), Brennweite in Pixeln mal 16 |
| `0xF000_306C` | `RS_SMG` | W | die Tabelle der Routine 224 (16384 Byte, 16 M10K): Bit 21..8 die Byte-Adresse (Zeile × 256 + altes Pixel), Bit 7..0 der neue Wert; die Firmware schreibt `MAPn.SMG` beim Streckenstart, Zeile 63 ist das Abdunkeln des Radars (× 0,75, `0x25870`); nicht während gemischt gezeichnet wird |
| `0xF000_3088`–`3090` | `SU_MODE`, `SU_TEX`, `SU_TEXSIZE` | W | wie `RS_DRAW` Bit 9..0, `RS_TEX`, `RS_TEXSIZE`, dazu in `SU_MODE` Bit 12..10 die Zahl der Ecken (3 oder 4); gelten ab der nächsten ersten Ecke für das ganze Vieleck |
| `0xF000_3094`–`30A0` | `SU_R`, `SU_U`, `SU_F`, `SU_S` | W | eine Ecke in Kamerasicht als Float: rechts, oben, vorn (nach der nahen Ebene, > 0), Texel-Spalte |
| `0xF000_30A4`, `30A8` | `SU_T_NEXT`, `SU_T_FIRST` | W | Texel-Zeile (Float) und Übergabe der Ecke: setzt das Vieleck fort bzw. beginnt ein neues. Mit der letzten Ecke geht das Vieleck an den Clipper: geschnitten an der nahen Ebene (Tiefe 128), am Schutzband (\|r\|, \|u\| ≤ 14 × Tiefe) und an der fernen Ebene, dann ein Fächer von Dreiecken aus der ersten Ecke. Der Schreibzugriff wartet, solange der Eingang noch das vorige Vieleck hält; `RS_DRAW` und Framebuffer-Zugriffe warten, bis Setup-Einheit und Rasterizer fertig sind, `RS_DRAW` Bit 0 zeigt beide |
| `0xF000_30AC` | `SU_CLOCKS` | R/W | Takte, in denen die Setup-Einheit gerechnet hat; Schreiben löscht |
| `0xF000_30B0` | `SU_FAR` | W | Tiefe der fernen Ebene mit 8 Nachkommabits (ganze Zahl) |
| `0xF000_3144`, `3148` | `GU_MLB`, `GU_MODE` | W | SDRAM-Byte-Adresse der Materialdatei (MLB); Bit 8 der Puffer, in den gezeichnet wird |
| `0xF000_314C` | `GU_GO` | W | Bit 2..0 die Flächenliste für die Seite des Auges, startet das Bauteil. Wartet, solange die Einheit am vorigen arbeitet; die Register davor dürfen schon für das nächste geschrieben werden. Die Einheit liest das Mesh selbst (4-KB-Lese-Cache, Zeilen über den Port des Rasterizers), rechnet die Eckpunkte um, lässt Rückseiten und Material 224 weg und gibt jede Fläche an die Setup-Einheit; Ecken der CPU an die Setup-Einheit warten, bis sie fertig ist |
| `0xF000_3150`, `3154` | `GU_FACES`, `GU_CLOCKS` | R/W | weitergegebene Flächen, Takte beschäftigt; Schreiben löscht |
| `0xF000_3158` | `GU_INVAL` | W | den Lese-Cache leeren (nachdem sich Mesh-Daten geändert haben) |
| `0xF000_315C` | `GU_PUSH` | W | Geometrie-Einheit (`geom.v`, bitgenau `firmware/geom.c`): das nächste Wort eines Bauteils in ihren Puffer (2048 Wörter, 107 Bauteile); wartet nur, wenn er voll ist. Ein Bauteil sind 19 Wörter: lokal nach Kamerasicht (Zeilen r, u, f der Spalten x, y, z, 16 Nachkommabits, innerhalb ±2^17), Ursprung in Kamerasicht und Auge im Rahmen des Bauteils (je 8 Nachkommabits), SDRAM-Byte-Adressen von Mesh-Objekt und Block, Bit 4 und 2..0 die Flächenliste für die Seite des Auges (0..15, gelesen bei Block +0x28 + 4 × Liste; Bit 4 zählt 8: das Auge unter der x-z-Ebene des Blocks, wie im Spiel bei `0x8a598`), Bit 3 ein Block eines beweglichen Teils (seine Bilder spiegeln ihn, die Rückseiten sind die anderen, wie im Spiel bei `0x8acf0`) und Bit 29..16 die Texturphase (von n Bildern einer Textur, Texturdatensatz +0, wird Bild n × Phase >> 14 gezeichnet, wie im Spiel: Streckentexturen nach der Animationsuhr, beim Auto Lackierung, Bremslicht und Radbild), Wort 18 für Routine 160 Bit 15..0 die Phase des Spiegelbilds (Bild n × Phase >> 16, Objektsatz +0x3c) und Bit 23..16, 31..24 die Versätze von u und v der Spiegelung (die CPU führt sie je Objekt wie `0xc1fc0`). Die Einheit nimmt ein Bauteil, sobald es ganz im Puffer liegt, liest das Mesh selbst (4-KB-Lese-Cache, Zeilen über den Port des Rasterizers), rechnet die Eckpunkte um, bei einem Block mit Flag 0x2000 für jeden Eckpunkt mit Normale die Spiegelung (u, v in einen RAM von 512, `firmware/geom.h`), lässt Rückseiten und Material 224 weg und gibt jede Fläche an die Setup-Einheit; Ecken der CPU an die Setup-Einheit warten, bis sie fertig ist |
| `0xF000_3180` | `BL_PUSH` | W | Blitter (`blit.v`, bitgenau `firmware/blit.c`): das nächste Wort eines Befehls in seinen Puffer (512 Wörter, 128 Befehle); wartet nur, wenn er voll ist. Ein Befehl sind 4 Wörter: Wort 0 die SDRAM-Byte-Adresse des ersten Quellpixels (Linie: Bit 9..0 x, 25..16 y ihres Endes), Wort 1 Bit 9..0 x, 25..16 y des ersten Pixels bzw. Linienanfangs (mit Vorzeichen), 29..28 die Art (0 Kopie, 1 Schrift, 2 Linie, 3 Füllen), 30 der Puffer, 31 bei Kopien deckend; Wort 2 Breite und Höhe (Linie: Clip links, oben), Wort 3 Bit 9..0 Bytes von Zeile zu Zeile (Linie: Bit 8..0 und 24..16 Clip rechts, unten, ausschließlich), 31..24 die Farbe. Kopie: das Byte, 0 durchsichtig (deckend: auch 0); Schrift: Schattierung 1..15 als Farbe × 16 + Schattierung; Linie: Bresenham, die Pixel im Clip; Füllen: das Rechteck in der Farbe. Kopien, Schrift und Füllen werden auf den Bildschirm beschnitten. Der Blitter beginnt einen Befehl nur, solange Geometrie-Einheit, Setup-Einheit und Rasterizer stillstehen: den Hintergrund vor der Szene (die Firmware hält das erste Bauteil zurück, bis er fertig ist), die Rennanzeige danach. Kopie und Schrift ein Pixel pro Takt, die Quelle zeilenweise über den gemeinsamen Zeilen-Port; Füllen ein Wort (vier Pixel) pro Takt |
| `0xF000_3184` | `BL_CLOCKS` | R/W | Takte, in denen der Blitter gearbeitet hat; Schreiben löscht |
| `0xF000_4000` + 32·v | `AU_ADDR(v)` | W | Mischer (`audio.v`, bitgenau `firmware/mixer.c`), Stimme v (0..31): SDRAM-Byte-Adresse des ersten Bytes ihres Samples (8 Bit vorzeichenlos). Ein Register wirkt, wenn der Durchgang zu seiner Stimme kommt; zwischen zwei Durchgängen geschrieben, alle zugleich |
| `0xF000_4004` + 32·v | `AU_END(v)` | W | Byte-Adresse, an der das Sample (bzw. seine Schleife) endet |
| `0xF000_4008` + 32·v | `AU_LOOP(v)` | W | Bit 19..0: am Ende geht die Stelle um so viele Bytes zurück; Bit 31..24: wie oft (0 nie, dann hört die Stimme am Ende auf; 255 immer) |
| `0xF000_400C` + 32·v | `AU_STEP(v)` | W | Schritt je Ausgabewert in Samples, 2.16 (unter 4): die Tonhöhe des Spiels (16.16 der eigenen 11025 Hz) × 11025 / 48000 |
| `0xF000_4010` + 32·v | `AU_VOL(v)` | W | Bit 7..0 links, 15..8 rechts: das zentrierte Byte (Byte − 128) mal diese Zahl |
| `0xF000_4400` | `AU_PLAY` | R/W | Schreiben: Bit v startet Stimme v (neu) an ihrem ersten Byte mit ihrer Schleifenzahl; der nächste Durchgang setzt die Stelle und holt ihre Zeile, der übernächste spielt das Byte. Lesen: Bit v = Stimme v spielt |
| `0xF000_4404` | `AU_STOP` | W | Bit v hält Stimme v an |
| `0xF000_4408` | `AU_STATUS` | R/W | Bit 0: ein Durchgang war bis zum nächsten Ausgabewert nicht fertig (der vorige Wert wurde wiederholt); Schreiben löscht |
| `0xF000_440C` | `AU_MUSIC_ADDR` | W | Musik: Bit 25..18 die SDRAM-Byte-Adresse ihres Rings (256 KB, je Wort ein Stereo-Wert der CD, links in der unteren Hälfte); vor dem Start setzen |
| `0xF000_4410` | `AU_MUSIC_VOL` | W | Bit 15..0 Lautstärke der Musik: Sample × Wert >> 15 |
| `0xF000_4414` | `AU_MUSIC` | R/W | Schreiben: Bit 0 = 1 spielt den Ring von vorn (die zwei Durchgänge danach laden seine Zeilen, der dritte spielt den ersten Wert), 0 hält an. Lesen: Bit 15..0 die Stelle (der Wert im Ring, ab dem der nächste Durchgang spielt), Bit 16 spielt |

## Ton

Der Mischer (`audio.v`) geht einmal je Ausgabewert (48 kHz) durch die 32 Stimmen, alle vier
Takte eine, mit einem gemeinsamen Rechenweg: das gespeicherte Byte an der Stelle der Stimme
(das nächstliegende Sample, nichts geglättet, wie der Treiber des Spiels), zentriert, mal
Lautstärke links und rechts, aufsummiert, auf 16 Bit begrenzt; dann rückt die Stelle um den
Schritt weiter, in ihre Schleife zurück oder bis zum Ende. Die Register stehen in MLABs, je
Stimme liegt die Zeile (32 Bytes) um ihre Stelle in einem M10K; wechselt die Stelle in eine
andere Zeile, holt der Durchgang sie über den Zeilen-Port (vor dem Rasterizer), lange bevor
der nächste sie braucht. Ausgabe als I²S an den Pocket im Takt der Vorlage: MCLK 12,288 MHz
aus einem Bruchteiler auf `clk_74a`, Bittakt MCLK / 4, 64 Bit je Wert, LRCK low links, high
rechts, je 16 Bit MSB zuerst ab dem zweiten Bittakt der Hälfte. Ein CPU-Reset hält alle
Stimmen an, die Ausgabe läuft weiter.

Nach den Stimmen spielt derselbe Durchgang die Musik, acht Takte: ein Strom 16-Bit-Stereo mit
44,1 kHz (die CD) aus einem Ring von 256 KB im SDRAM, den die Firmware füllt. Je Seite die zwei
Werte um die Stelle, dazwischen auf einer Geraden (16 Bit Bruchteil, abgerundet), mal
Lautstärke (>> 15) in die Summen; dann rückt die Stelle um 60211 / 65536 Werte weiter (44.100 /
48.000, 3 ppm zu langsam). Die zwei Zeilen um die Stelle (16 Werte) liegen in einem MLAB; rückt
die Stelle in die zweite, holt der Durchgang die danach über den Zeilen-Port, einmal je acht
Werte. Die vier Produkte rechnet der Multiplizierer der Stimmen, dafür 18 × 18 Bit breit; die
Summen haben 22 Bit. Die Firmware (`firmware/music.c`) füllt den Ring aus `SCREAM.gog` (Slot 22):
beim Start eines Lieds ganz, danach einmal je Bild mit einem Lesebefehl von 32 KB, den sie nur
startet (`slot_start`); `AU_MUSIC` sagt ihr, wie weit der Mischer ist. Das kostet die CPU rund
500 Takte je Bild (`docs/bildablauf.svg`, „Musik“), die Bridge schreibt 176 KB/s in den SDRAM.
In der Simulation ist der Ausgang 4,9 s lang Wert für Wert der CD-Track (`tools/musiccheck.py`).

Die Firmware (`firmware/sound.c`) spielt die Effekte aus `SFXSBASE.DTR` (GEN.DAT): die Samples
liegen ungepackt in `SOUND.DAT`, der Mischer liest sie direkt aus dem geladenen Container.
Lautstärke: die Zeile der Liste mal der Hauptregler für Effekte, `0x4000` von `0x7fff`, zwei
Drittel des Werts im Spiel (`0x6000`, `0x1bc2f`): so bleiben der Motor des Spielers und die
drei der Gegner bei voller Lautstärke unter 16 Bit, auch wenn alle Samples zugleich am
Anschlag sind (höchstens 32.000). Ein volles Sample bewegt den Ausgang um 128 je 8-Bit-Stufe.

Geräusche mit einem Ort (OpenScreamers Notes Abschnitte 15 und 17) hört man an der Kamera, mit
dem Abstand d in x und y; `car_step` setzt sie jeden Schritt (`snd_ear`). Die Motoren der
Gegner (`oppmotor`, `0x1dffc`, `opp.c`): nur Autos innerhalb von 35000 Einheiten und nur die
drei nächsten, Lautstärke ((35000 − d) / 35000)² × 23552, Tonhöhe Tempo × 2,4 + 13568; bis
Grün gilt als Tempo sin(Autonummer × 0,5 × Uhr) × 15000, die Gegner lassen die Motoren
aufheulen (`0x20e63`; unter der kleinsten Tonhöhe bleibt die Stimme stehen). Die beweglichen
Teile der Strecke (`0x21b80`, `scenery.c`): die vierte Spalte von `ANIMATIONS` nennt eine
Zeile der eigenen `SOUND_FX`-Liste in `MAPnDAT.DTR` (Strecke 1: `truck`, `airplane`), die
hinter den Effekten und dem Sprecher geladen wird; innerhalb von 40000 Einheiten, Lautstärke
((40000 − d) / 40000)², Tonhöhe 65536. Bei beiden kommt der Doppler dazu, + 9 je Einheit, die
die Quelle in einem Bild des Spiels (1/30 s) näher kam, hier je Schritt × 50 / 30; springt die
Kamera in einem Schritt über 1000 Einheiten (ein Schnitt), entfällt er. Nur diese Geräusche
liegen im Stereobild: die Linie zur Quelle im Rahmen der Kamera, pan = 0x8000 ± 32767 ×
√(|seitlich| / d), die ferne Seite linear leiser; die eigenen Geräusche des Spielers bleiben in
der Mitte.

Der Sprecher (`firmware/speech.c`, OpenScreamers Notes Abschnitt 17, „commentary“; EXE
`0x2479c`, `0x24820`, `0x24a24`, `0x2425c`): seine Aufnahmen sind die Zeilen von `SPFXBASE.DTR`
und `SPFXFULL.DTR` (GEN.DAT, 11 + 47), hinter den Effekten geladen; die Tabelle des Spiels
(`0x1fb0`) gibt jedem der 27 Ereignisse bis zu drei davon. Ein Ereignis kommt mit Modus
(Aufnahme 0–2 oder reihum) und Verzögerung in eine Warteschlange, jedes höchstens einmal.
Schweigt die Stimme, läuft eine Uhr; das Ereignis mit der kürzesten Verzögerung kommt dran,
sobald die Uhr sie erreicht, und nach dem Satz beginnt die Uhr von vorn, so überlappen sich
nie zwei Sätze. Auslöser: der Countdown mit den Startampeln (`0x20d45`: 0, 2, 4, 6 s),
Checkpoint (Aufnahme nach der Restzeit), Ziellinie (Runde; „new lap record“ unter dem Rekord,
ohne `CHOICE.DAT` die 180 s des Spiels), Crash, das Ende (gewonnen, verloren, Zeit um) und je
Schritt die Wächter von `0x2425c` mit ihren Zeiten: falsche Richtung (gegen die Straße an der
letzten Wand, `+0x13c`), Stillstand, Eile, Führung und ihr Verlust, Kurve (Marker-Flag 4),
Rutschen, Letzter, eine Runde voraus, Berührung mit einem Gegner. Die Stimme steht in der Mitte,
mit eigenem Hauptregler `0x3000` (im Spiel beginnen Effekte, Sprecher und Musik alle bei
`0x6000`; die Aufnahmen sind bis zum Anschlag ausgesteuert). Mit `0x4000` wie die Effekte
übersteuerte der Countdown über den aufheulenden Motoren am Start und der Musik in der Simulation
(7 Werte); mit `0x3000` (die Stimme 2,5 dB unter dem Verhältnis des Spiels) bleibt die Spitze
bei 28.631 von 32.767. Jeder Satz
geht ins Debug-Log, wenn er zu Ende ist: `say <von>-<bis> <Ereignis> <Aufnahme>` (Rennuhr),
solange die 4 KB Platz haben; nach dem Bericht des Startbilds und der Zeile alle 250 Bilder
ist es voll.

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

**Spielstände** gehen denselben Weg, liegen aber im SDRAM: die nichtflüchtigen Data-Slots
`CHOICE.DAT` (id 23, `0x13F0_0000`, 1120 Byte: die Einstellungsdatei des Originals mit
Optionen, Meisterschaft, Rekorden und Freischaltungen) und `ATTACK.DAT` (id 24, `0x13F1_0000`,
6 × 60.016 Byte: je Strecke der behaltene Lauf des Zeitfahrens, `struct ghost_file` in
`ghost.h`, den `ghost.c` direkt dort liest und schreibt). Beide sind `deferload`: Die
Firmware liest sie gleich nach dem Start (`save_load` in `save.c`), ohne Datei oder bei einem
`CHOICE.DAT`, das nicht wie das des Originals aussieht, nimmt sie die Vorgaben des Originals
(`0x1bbb8`), und trägt dann mit `DS_SIZE` die volle Größe in die Slot-Tabelle ein, damit der
Pocket sie beim Beenden speichert. Von da an ist jedes Schreiben der CPU dorthin (`choice`
in `champ.h`) schon der Spielstand; der Pocket liest ihn beim Beenden über die Bridge aus
dem SDRAM (`sdram_port.v`: ein Lesewort der Bridge geht gleich nach dem Schreibpuffer der
Bridge an den Controller und hat bis zum nächsten Bridge-Wort, mindestens 88 Takte, Zeit).
Beendet man den Core, bevor `save_load` gelaufen ist (in der ersten Sekunde), speichert der
Pocket, was dort im SDRAM steht.

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
- Die Antwort der FPU auf einen Vergleich (`flt.s`, `fle.s`, `feq.s`) oder eine Umwandlung in
  eine ganze Zahl (`fcvt.w.s`) kommt spät zurück; ein Befehl, der sie gleich danach braucht,
  wartet. Mehrere Fragen nacheinander gestellt kommen nebeneinander zurück, die CPU wartet
  einmal: `firmware/track.c` (`INT_NOW…`) und `firmware/opp.c` (`ASKED…`) halten GCC mit einem
  leeren `asm`, das die Antworten als Operanden nimmt, davon ab, jede einzeln zu verbrauchen.
- GCC übersetzt `fminf`/`fmaxf` hier mit einem Aufruf von `__issignalingf` vor dem `fmin.s`;
  in einer Schleife langsamer als Vergleich und Sprung.
- picolibcs `memset` und `memcpy` arbeiten Byte für Byte; auch `= { 0 }` eines großen lokalen
  Felds wird zu einem `memset`-Aufruf.

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

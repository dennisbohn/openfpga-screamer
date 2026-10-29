# Screamer für den Analogue Pocket

Ziel: *Screamer* (Graffiti / Virgin Interactive, 1995, MS-DOS) als openFPGA-Core für den
Analogue Pocket, mit so viel Leistung wie möglich. Die Spiellogik läuft auf einem
RISC-V-Softcore, das Rendern übernehmen eigene Hardware-Einheiten.

Grundlage für Spiellogik und Dateiformate ist [OpenScreamer](https://github.com/ItsDanik/openscreamer)
(GPL-3.0). Die Spieldaten sind nicht enthalten, man braucht eine eigene Kopie des Spiels.

## Aufbau

| Pfad | Inhalt |
|---|---|
| `core/` | der Pocket-Core, aus dem [Core-Template von Analogue](https://github.com/open-fpga/core-template) (v1.3.0) |
| `core/src/fpga/` | Quartus-Projekt (`ap_core.qpf`), `apf/` = Analogue-Rahmen, `core/` = unser Teil |
| `tools/` | Hilfsprogramme (Build, Bitstream umdrehen, Spieldaten untersuchen) |
| `docs/` | Architektur und Entscheidungen |

## Bauen

Quartus läuft in Docker (Image `raetro/quartus:pocket`, Quartus Prime Lite 18.1.1, dieselbe
Version wie im Template). Ein Build dauert etwa 6 Minuten; am Ende stehen drei Zeilen
(`tools/buildsum.py`): Bauzeit und Belegung, die knappste Reserve jeder Timing-Prüfung und die
Logik jeder Einheit, jeweils mit der Änderung zum vorigen Build. Das volle Log liegt in
`build/quartus.log`:

```sh
tools/build.sh
```

Ergebnis: `core/output/bitstream.rbf_r`. Der Pocket braucht den Bitstream bitweise
umgedreht, das erledigt `tools/reverse_rbf.py`.

```sh
tools/package.sh
```

stellt in `build/sdcard/` die Ordner für die SD-Karte zusammen (`Cores/Chainy.Screamer/`,
`Platforms/`). Den Inhalt auf die oberste Ebene der SD-Karte kopieren, oder direkt
mit `tools/deploy.sh /i` (SD-Karte bzw. Pocket im USB-Modus als Laufwerk I:).

## Spieldaten für die SD-Karte

Der Core braucht nur die originalen Spieldateien, unverändert: die Ordner `DATA`, `GFX`
und `TABS` aus dem Spielordner (etwa der GOG-Fassung) so, wie sie sind, nach
`Assets/screamer/common/` auf der SD-Karte kopieren. Heute liest er daraus
`GFX/MAP1H.DAT`, `GFX/GAME.DAT`, `DATA/MAP1.DAT`, `DATA/GEN.DAT`, `DATA/LOCAL.DAT` und
`DATA/SOUND.DAT`. Nichts wird auf dem PC umgewandelt: Die Firmware liest die Container
(Data-Slots 3 bis 8),
entpackt sie und baut daraus beim Start das Abbild der Strecke (Platzierungen, Palette,
Kamerapfad, die befahrbaren Teile je Rasterzelle, Marker, Wände, die Autos aus `CARS.DTR`,
die Rennanzeige, das Hintergrundbild `MAP1.BCK`, die Linien und Werte der Gegner aus
`MAP1OPP.SPL` und `MAP1B.DTR`). `tools/deploy.sh <SD-Karte> [Spielordner]` kopiert die
eigenen Spieldateien mit (Vorgabe `C:/GOG Games/Screamer`), `tools/package.sh` für eine
Veröffentlichung nie. `firmware.bin` und `program.bin` kommen aus `tools/fw.sh` (siehe
`firmware/README.md`).

`tools/make_track.py` baut dasselbe Abbild auf dem PC; es dient nur noch als Vergleich für
`tools/check_track_build.sh <Spielordner>`, das die Firmware-Bausteine nativ übersetzt und
Byte für Byte vergleicht.

`L` und `H` sind die Sätze der Option DETAIL, nicht die Auflösung: Das Spiel lädt den
`L`-Satz nur bei LOW, ab MID den `H`-Satz (mehr Kulisse, dieselben Texturen). Der Durchflug
zeichnet wie bei HI, der Voreinstellung des Spiels.

Ohne diese Datei zeigt der Core eine kleine Szene mit Testmustern.

## Simulation

Verilator läuft in Docker (`verilator/verilator:v5.050`). Jede Testbench liegt in
`sim/<name>/`: `files.f` nennt das oberste Modul und die Quellen, `tb.cpp` treibt sie.

```sh
# Zwei Framebuffer: zwei Bilder aus den Spieldaten über die Bridge laden, per Taste
# tauschen, 13 Bilder aufnehmen und 12 davon Pixel für Pixel prüfen (eines unterbricht ein
# Reset): Tausch nur in der Austastlücke, Palette je Puffer, nach dem Reset Puffer 0 ohne
# Tausch. Dann der Zeichen-Port (Lesen, Byte-Enables).
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD2L.PCX \
    build/sim/screen.bin build/preview/LOAD2L.png
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD1L.PCX \
    build/sim/screen2.bin build/preview/LOAD1L.png
tools/sim.sh display build/sim/screen.bin build/sim/screen2.bin build/sim/display_frame

# CPU-System mit Firmware (docs/soc.md, docs/speicher.md) und einem Modell des SDRAM:
# Selbsttests, Speichertest, Prüfstreifen, Protokoll über die Bridge, dann zwei
# Resets mit neu geladener Firmware (einer mitten im Speichertest).
tools/fw.sh
tools/sim.sh soc build/firmware/firmware.bin build/sim/soc
# Fehlertest: Befehlsabruf hinter dem Boot-RAM, der Trap-Handler muss ihn melden.
tools/fw.sh FAULT=IFETCH
tools/sim.sh soc build/firmware-IFETCH/firmware.bin build/sim/soc_ifetch 80 c0000001
# Gegenproben: je ein Fehler in einem Speichermodell, der Speichertest muss genau die
# erwarteten Tests als fehlgeschlagen melden (sieben Läufe parallel).
tools/sim_faults.sh

# Zufallstest von SDRAM-Port und -Controller gegen das SDRAM-Modell: Seed, Takte.
tools/sim.sh sdram 1 2000000
# Selbsttest des SDRAM-Modells: je Regel ein richtiger und ein falscher Ablauf.
tools/sim.sh sdram_model
# Der Mischer (audio.v) gegen sein C-Modell firmware/mixer.c: zufällige Stimmen, Schleifen,
# Schritte und Lautstärken, Zeilen mit zufälliger Wartezeit; verglichen wird jeder Ausgabewert,
# wie er über I²S hinausgeht. Ausgabewerte, Seed.
tools/sim.sh audio 20000 1
```

Die Speichermodelle in `sim/models/` (`sdram_model.v`, `sram_model.v`, `psram_model.v`)
prüfen die Zeiten aus den Datenblättern in Takten und lassen sich für die Gegenproben mit
Fehlern bauen (`SIM_DEFINES`, siehe `tools/sim.sh`).

## Werkzeuge

- `tools/dat_sizes.py <datei.DAT>...` listet den Inhalt der Spiel-Container mit
  entpackten Größen (`-v` für jede Datei einzeln).
- `tools/datfile.py` liest die Container (gemeinsam genutzt von den anderen Skripten).
- `tools/make_budget_svg.py` zeichnet `docs/ressourcen.svg` aus der Zahlentabelle darin.
- `tools/deploy.sh /i` packt den Core und kopiert ihn auf die SD-Karte.
- `tools/gen_pll_sdram.sh` erzeugt die PLL für den SDRAM-Takt neu (Intels `ip-generate` im
  Quartus-Image).
- `tools/sta_sdram.tcl` prüft nach jedem Build die vier SDRAM-IO-Übergänge (von
  `tools/build.sh` aufgerufen), `tools/check_budget.py` Belegung und IO-Register.
- `tools/simcheck.sh [--full] [--save] [name]` lässt die SoC-Simulation mit den Spieldateien
  aus `build/gamedata/` laufen und fasst sie in wenigen Zeilen zusammen (`tools/simsum.py`):
  Ergebnis, Pixelvergleich mit den C-Modellen, die Zählerstände des ersten Rennbilds mit der
  Änderung zum gespeicherten Vergleichsstand (`--save`). Ohne `--full` der Schnelllauf: ohne
  die zwei Resets und mit dem fertigen Streckenabbild als Slot der Testbench, den das
  Programm nur in der Simulation findet; `--full` wie auf dem Pocket. Den Ton des Laufs
  (die I²S-Ausgabe, 48 kHz Stereo) schreibt sie nach `build/sim/audio.wav`; zum Anhören mit
  Gas: `SIM_KEYS=20 SIM_RACE_FRAMES=75 tools/simcheck.sh drive`. `build/gamedata/` braucht
  dafür auch `DATA/SOUND.DAT`.
- `tools/pocketlog.py [debug.txt]` liest das Protokoll des Pocket (Vorgabe: die SD-Karte als
  I:) als kurze Tabelle: Laden, Prüfungen, Fehler, je Probe Tempo, Flächen, fps und Bildzeit.
- `tools/dis.sh [-h] [-f] <von> <bis>` disassembliert ein Stück von `STARTL.EXE` (`-h`:
  `STARTH.EXE`) an den Adressen von OpenScreamers Notes; das entpackte Programm legt
  `tools/unle.py` (aus OpenScreamer) beim ersten Aufruf in `build/exe/` ab.
- `tools/check_track_build.sh <Spielordner>` vergleicht das Streckenabbild der Firmware
  (nativ übersetzt) Byte für Byte mit dem von `tools/make_track.py`.

## Lizenz

GPL-3.0, siehe `LICENSE`. Die Dateien aus dem Core-Template von Analogue (`core/src/fpga/apf/`
und die Vorlagen in `core/`) stammen von Analogue.

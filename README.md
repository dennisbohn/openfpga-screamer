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
Version wie im Template). Ein Build dauert etwa eine Minute, das Skript gibt am Ende Belegung
und Timing aus, das volle Log liegt in `build/quartus.log`:

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

Was der Core aus den Spieldaten braucht, wird auf dem PC umgewandelt und landet in
`build/assets/` (nie im Repository), `tools/package.sh` kopiert es nach `Assets/`.
Dort liegen `firmware.bin` aus `tools/fw.sh` (siehe `firmware/README.md`) und für die
Textur-Vorführung `textures.bin`, ein paar Texturen und die Palette von Strecke 1 (Data-Slot 3):

```sh
python tools/make_textures.py "C:/GOG Games/Screamer/GFX/MAP1L.DAT" \
    build/assets/screamer/Chainy.Screamer/textures.bin build/preview/textures.png 26 45 36 20 0
```

Ohne diese Datei zeigt der Core einfarbige Dreiecke.

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

# CPU-System mit Firmware (docs/soc.md, docs/speicher.md) und Modellen von SDRAM, SRAM und
# PSRAM: Selbsttests, Speichertest, Prüfstreifen, Protokoll über die Bridge, dann zwei
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

## Lizenz

GPL-3.0, siehe `LICENSE`. Die Dateien aus dem Core-Template von Analogue (`core/src/fpga/apf/`
und die Vorlagen in `core/`) stammen von Analogue.

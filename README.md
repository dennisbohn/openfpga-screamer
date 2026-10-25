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
Zurzeit sind das zwei Bilder für den Framebuffer-Test (Taste A tauscht sie):

```sh
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD2L.PCX \
    build/assets/screamer/Chainy.Screamer/screen.bin build/preview/LOAD2L.png
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD1L.PCX \
    build/assets/screamer/Chainy.Screamer/screen2.bin build/preview/LOAD1L.png
```

## Simulation

Verilator läuft in Docker (`verilator/verilator:v5.050`). Jede Testbench liegt in
`sim/<name>/`: `files.f` nennt das oberste Modul und die Quellen, `tb.cpp` treibt sie.

```sh
# Zwei Framebuffer: beide Bilder über die Bridge laden, mit Taste A tauschen,
# acht Bilder Pixel für Pixel prüfen (Tausch nur an der Bildgrenze, Palette je Puffer).
tools/sim.sh display build/assets/screamer/Chainy.Screamer/screen.bin \
    build/assets/screamer/Chainy.Screamer/screen2.bin build/sim/display_frame
```

`sim/models/psram_model.v` ist ein Verhaltensmodell des PSRAM (prüft die 70-ns-Zugriffszeit),
für den späteren Textur-Speicher.

## Werkzeuge

- `tools/dat_sizes.py <datei.DAT>...` listet den Inhalt der Spiel-Container mit
  entpackten Größen (`-v` für jede Datei einzeln).
- `tools/datfile.py` liest die Container (gemeinsam genutzt von den anderen Skripten).
- `tools/make_budget_svg.py` zeichnet `docs/ressourcen.svg` aus der Zahlentabelle darin.
- `tools/deploy.sh /i` packt den Core und kopiert ihn auf die SD-Karte.

## Lizenz

GPL-3.0, siehe `LICENSE`. Die Dateien aus dem Core-Template von Analogue (`core/src/fpga/apf/`
und die Vorlagen in `core/`) stammen von Analogue.

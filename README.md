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
`Platforms/`). Den Inhalt auf die oberste Ebene der SD-Karte kopieren.

## Werkzeuge

- `tools/dat_sizes.py <datei.DAT>...` listet den Inhalt der Spiel-Container mit
  entpackten Größen (`-v` für jede Datei einzeln).

## Lizenz

GPL-3.0, siehe `LICENSE`. Die Dateien aus dem Core-Template von Analogue (`core/src/fpga/apf/`
und die Vorlagen in `core/`) stammen von Analogue.

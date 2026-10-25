# Fremde IP

| Datei | Herkunft | Lizenz |
|---|---|---|
| `psram.sv` | [agg23/analogue-pocket-utils](https://github.com/agg23/analogue-pocket-utils) `ip/mem/psram.sv`, Commit `78482d1`, unverändert | MIT (Kopf der Datei) |

`psram.sv` ist ein asynchroner Controller für die CellularRAM-Chips des Pocket
(AS1C8M16PL-70, `cram0`/`cram1`). Der PlayStation-Core für den Pocket nutzt ihn
unverändert auf echter Hardware, und bei uns trug er den Framebuffer in `cram1`, bis die
Framebuffer in den Block-RAM gewandert sind (Commit `b148783` lief so auf dem Pocket).
Zurzeit nicht eingebunden; vorgesehen für den Textur-Speicher in `cram0`.

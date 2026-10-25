# Fremde IP

| Datei | Herkunft | Lizenz |
|---|---|---|
| `psram.sv` | [agg23/analogue-pocket-utils](https://github.com/agg23/analogue-pocket-utils) `ip/mem/psram.sv`, Commit `78482d1`, unverändert | MIT (Kopf der Datei) |

`psram.sv` ist ein asynchroner Controller für die CellularRAM-Chips des Pocket
(AS1C8M16PL-70, `cram0`/`cram1`). Auf echter Hardware läuft er unverändert in agg23s
eigenen Cores (z. B. `openfpga-SNES`). Der PlayStation-Core für den Pocket betreibt seine
PSRAMs dagegen synchron, anders als früher hier stand. Bei uns trug `psram.sv` den
Framebuffer in `cram1`, bis die Framebuffer in den Block-RAM gewandert sind (Commit
`b148783` lief so auf dem Pocket). Zurzeit nicht eingebunden; vorgesehen für den
Textur-Speicher in `cram0`.

## Für den Textur-Speicher in `cram0` beachten

Aus der Hardware-Prüfung des alten PSRAM-Framebuffers (2026-10-25):

- **Konfiguration nicht voraussetzen.** BCR/RCR des Chips überleben das Laden eines
  anderen Cores, nur Ausschalten setzt sie zurück. Asynchrone Zugriffe mit stehendem
  `cram_clk` funktionieren laut Datenblatt in jedem Modus; wer den Burst-Modus nutzt, muss
  BCR (und RCR) ohnehin selbst schreiben, und zwar nach dem Einschalten jedes Mal.
- **IO-Timing festmachen.** Die `cram`-Ausgänge in IO-Register legen
  (`FAST_OUTPUT_REGISTER`, für `cram_dq` auch `FAST_OUTPUT_ENABLE_REGISTER` und
  `FAST_INPUT_REGISTER`, wie `markus-zzz/myc64-pocket`), sonst hängt das Ein-Takt-Timing von
  der Platzierung ab.
- **Startwerte der Steuerleitungen.** Initialwerte in der Port-Deklaration von `psram.sv`
  (`cram_adv_n = 1` usw.) übernimmt Quartus 18.1 womöglich nicht; dann die
  `POWER_UP_LEVEL HIGH`-Zuweisungen in der `.qsf` setzen.
- **Simulation:** `sim/models/psram_model.v` mit einem Giftmuster statt Nullen vorbelegen
  und prüfen, dass jede Adresse geschrieben wurde; der Framebuffer-Test macht das schon so.

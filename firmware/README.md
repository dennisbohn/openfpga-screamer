# Firmware (Schritt 3: Dreiecke)

Die Programme für die RISC-V-CPU im Core, in zwei Teilen: `firmware.bin`, der Lader im
Boot-RAM (`boot.c`: Selbsttests, Speichertest, Prüfung und Start des Programms), und
`program.bin`, das Programm im SDRAM (`main.c`), das über die Caches läuft. Speicherkarte,
Register, Startablauf und was die Simulation prüft, stehen in [`docs/soc.md`](../docs/soc.md),
der Speichertest in [`docs/speicher.md`](../docs/speicher.md).

Beim Start löscht die Firmware das Protokoll (`debug.txt`), trägt die Prüfsumme ihres
eigenen Abbilds ein und prüft die CPU (Ganzzahl-Multiplikation, -Division und
Schiebebefehle, Fließkomma, CFU). Dann läuft der Speichertest über den SDRAM: normal kurz
(unter einer Sekunde, die ersten und letzten 2 KB), über den ganzen Speicher, wenn man beim
Start **Select** hält. Danach zeichnet sie
jedes Bild in den hinteren Puffer: Palette nach der Formel aus `docs/soc.md`, Prüfstreifen in
den oberen 8 Zeilen, zwei sich drehende Räder aus Dreiecken, die sich überdecken, und ein
Dreieck, das sich mit dem Steuerkreuz bewegt, dazu Pixel und Takte pro Bild. Die Dreiecke
zeichnet der Hardware-Rasterizer (`raster.v`); einmal je Lauf zeichnet sein C-Modell `raster.c`
dieselben Dreiecke ins SDRAM, und die Zeile `raster check` im Protokoll meldet, ob beide
Bilder Pixel für Pixel gleich sind. Hält man **Select**,
stehen statt der Dreiecke die Ergebnisse des Speichertests da. Nach drei getauschten Bildern
meldet sie alle Ergebnisse an `SYS_SIM` (`0x80001FFF`, wenn alles stimmt). Ein Absturz
(Trap) landet mit `mcause`, `mepc` und `mtval` im Protokoll und unten auf dem Bildschirm.

## Bauen

```sh
tools/fw.sh
```

GCC 14 und picolibc aus Debian trixie laufen in Docker (`firmware/docker/Dockerfile`,
Basis-Image und Paketstand festgenagelt); das Image `screamer-fw:trixie-20261005` baut das
Skript beim ersten Mal selbst. Ergebnis in `build/firmware/`: `firmware.bin` (höchstens
32 KB, das Skript bricht sonst ab), `firmware.elf`, `firmware.lst` (Disassembly) und
`firmware.map`. `firmware.bin` wird nach `build/assets/screamer/Chainy.Screamer/` kopiert,
`tools/package.sh` bzw. `tools/deploy.sh` bringen es von dort auf die SD-Karte.

| Datei | Inhalt |
|---|---|
| `start.S`, `link.ld` | Lader: Sprung bei `0x0`, Trap-Einsprung bei `0x20`, alles im Boot-RAM unterhalb von 24 KB |
| `boot.c` | Lader: Selbsttests, Speichertest, Prüfung und Start von `program.bin` |
| `start_prog.S`, `link_prog.ld` | Programm: Kopf (Größe, CRC-32, Einsprung), Start, eigener Trap-Einsprung, im SDRAM ab `0x1300_0000` |
| `prog_crc.py` | trägt beim Bauen den CRC-32 in den Kopf von `program.bin` ein |
| `main.c` | Programm: Textur-Szene Bild für Bild (Hardware, einmal gegen `raster.c` geprüft), FPS |
| `hw.h` | Adressen und Register aus `docs/soc.md`, `rdcycle`, `fixmul` (CFU), Cache-Befehle |
| `selftest.c` | die drei Selbsttests mit festen Sollwerten |
| `memtest.c` | der Speichertest (`docs/speicher.md`) |
| `raster.c` | Rasterizer als C-Modell: Dreiecke mit genauer Pixelregel, Texturen in Fest- und Fließkomma |
| `texdemo.c` | die Szene (Fahne, Banner, Schild, Leitplanke), Testmuster ohne `textures.bin` |
| `screen.c` | was Lader und Programm beide zeichnen |
| `console.c` | Text (8×8-Font) und Rechtecke im Framebuffer, Zahlen ohne `printf` |
| `dlog.c` | das Protokoll `debug.txt` |
| `trap.c` | Trap-Handler |
| `font8x8_basic.h` | Font von [dhepper/font8x8](https://github.com/dhepper/font8x8) (gemeinfrei), unverändert |

## Protokoll vom Pocket holen

Der Pocket speichert den Protokollbereich, wenn man den Core im Menü beendet („Quit“) oder
ausschaltet, auch nach einem Absturz der CPU. Danach den Pocket per USB als Laufwerk
anschließen (oder die SD-Karte einstecken) und `/Saves/screamer/Chainy.Screamer/debug.txt`
öffnen. Die alte `debug.txt` lädt der Pocket beim Start zwar in den Bereich, der Core
ignoriert das aber: Nach dem Start des Cores stammt der Inhalt von diesem Lauf (besteht die
Datei nur aus Nullen, ist die CPU gar nicht losgelaufen). Ausnahme: Lädt man die Firmware im
Menü neu, ist das nur ein Reset, und das Protokoll des vorigen Laufs bleibt stehen, bis die
neue Firmware es löscht; welche Firmware es geschrieben hat, zeigt die Prüfsumme in der
zweiten Zeile. Beispiel aus der Simulation (gekürzt; auf dem Pocket steht „the whole memory"
statt der Fenster, und die Phasenkarte hat 80 Zeichen):

```
Screamer Pocket step 2, frames drawn: 6
firmware.bin: 19968 bytes, crc32 cc055ef4
SYS_ID=53435231 ok
selftest integer: ok
selftest float: ok
selftest cfu: ok
memtest: SYS_FLAGS 00000001, MEM_STATUS 00000013, bridge words 4992
SDRAM slot: ok, cycles 474618
phase map ####
phase: good from step 0, 4 steps of 168 ps, now at step 0
SDRAM phase: ok, cycles 37292
SDRAM: simulation, windows of 2048 bytes at the start and the end
SDRAM data bus: ok, cycles 14085
...
SDRAM line read: row hit 30 cycles, row miss 34; read 63 MB/s, write 95 MB/s
...
memtest: errors SDRAM 0
stack used 624 bytes
SYS_SIM=80001fff
```

Die Phasenkarte zeigt je Schritt von 168 ps ein `#` (SDRAM arbeitet) oder `.`; die Firmware
stellt die Mitte der längsten Folge ein. Ein fehlgeschlagener Speichertest nennt je Fehler
Adresse, Soll, Ist und zweimal nachgelesen (höchstens acht Zeilen je Speicher), zum Beispiel
`  bytes @10000080 want 5aa5c35e got 5aa55e3c 5aa55e3c 5aa55e3c`, und am Ende der Testzeile
„FAIL, errors n".

Die erste Zeile schreibt die Firmware nach jedem Bild neu: Steht der Zähler, hing sie dort.
Die Prüfsumme muss die sein, die `tools/fw.sh` beim Bauen ausgibt, sonst kam nicht genau
diese Datei im Boot-RAM an. Ein fehlgeschlagener Test nennt Prüfung, Ist- und Sollwert, ein
Absturz sieht so aus:

```
TRAP mcause=00000005 load access fault
     mepc=00000178 mtval=f0000000 ra=00000174 sp=00003f60
```

`mepc` ist die Adresse des Befehls, der den Trap ausgelöst hat; in `build/firmware/firmware.lst`
nachschlagen (dieselbe Firmware-Version vorausgesetzt).

# Firmware (Schritt 1: die CPU zeichnet)

Das erste Programm für die RISC-V-CPU im Core. Speicherkarte, Register und was die
Simulation prüft, stehen in [`docs/soc.md`](../docs/soc.md).

Beim Start löscht die Firmware das Protokoll (`debug.txt`), trägt die Prüfsumme ihres
eigenen Abbilds ein, prüft die CPU (Ganzzahl-Multiplikation, -Division und Schiebebefehle,
Fließkomma, CFU) und misst mit `rdcycle`, wie viele Takte 1000 Multiplizier-Additionen in
Fließkomma und in Ganzzahl brauchen. Danach zeichnet sie jedes Bild in den hinteren Puffer:
Palette nach der Formel aus `docs/soc.md`, Prüfstreifen in den oberen 8 Zeilen, Text mit den
Ergebnissen, Bildzähler und Tasten, dazu ein Quadrat, das sich mit dem Steuerkreuz bewegt.
Nach drei getauschten Bildern meldet sie das Ergebnis der Selbsttests an `SYS_SIM`
(`0x80000007`, wenn alles stimmt). Ein Absturz (Trap) landet mit `mcause`, `mepc` und
`mtval` im Protokoll und unten auf dem Bildschirm.

## Bauen

```sh
tools/fw.sh
```

GCC 14 und picolibc aus Debian trixie laufen in Docker (`firmware/docker/Dockerfile`,
Basis-Image und Paketstand festgenagelt); das Image `screamer-fw:trixie-20261005` baut das
Skript beim ersten Mal selbst. Ergebnis in `build/firmware/`: `firmware.bin` (höchstens
16 KB, das Skript bricht sonst ab), `firmware.elf`, `firmware.lst` (Disassembly) und
`firmware.map`. `firmware.bin` wird nach `build/assets/screamer/Chainy.Screamer/` kopiert,
`tools/package.sh` bzw. `tools/deploy.sh` bringen es von dort auf die SD-Karte.

| Datei | Inhalt |
|---|---|
| `start.S`, `link.ld` | Sprung bei `0x0`, Trap-Einsprung bei `0x20`, alles in den 16 KB Boot-RAM |
| `hw.h` | Adressen und Register aus `docs/soc.md`, `rdcycle`, `fixmul` (CFU) |
| `main.c` | Ablauf: Selbsttests, Messung, Bild für Bild zeichnen |
| `selftest.c` | die drei Selbsttests mit festen Sollwerten |
| `console.c` | Text (8×8-Font) und Rechtecke im Framebuffer, Zahlen ohne `printf` |
| `dlog.c` | das Protokoll `debug.txt` |
| `trap.c` | Trap-Handler |
| `font8x8_basic.h` | Font von [dhepper/font8x8](https://github.com/dhepper/font8x8) (gemeinfrei), unverändert |

## Protokoll vom Pocket holen

Der Pocket speichert den Protokollbereich, wenn man den Core im Menü beendet („Quit“) oder
ausschaltet, auch nach einem Absturz der CPU. Danach den Pocket per USB als Laufwerk
anschließen (oder die SD-Karte einstecken) und `/Saves/screamer/Chainy.Screamer/debug.txt`
öffnen. Eine alte `debug.txt` lädt der Pocket beim Start wieder in den Bereich; vor einem
Test löschen, dann stammt der Inhalt sicher von diesem Lauf (steht danach kein Text darin,
ist die CPU gar nicht losgelaufen). Beispiel:

```
Screamer Pocket step 1, frames drawn: 6
firmware.bin: 9992 bytes, crc32 e181cb69
SYS_ID=53435231 ok
selftest integer: ok
selftest float: ok
selftest cfu: ok
cycles: 1000 x fmadd.s 15002, 1000 x mul + add 6004
frame 3: draw 138935 cycles, background 67614 (15360 word stores)
SYS_SIM=80000007
```

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

# VexRiscv

`PocketVex.scala` konfiguriert den [VexRiscv](https://github.com/SpinalHDL/VexRiscv),
`gen.sh` erzeugt daraus Verilog. Die CPU des Cores, `VexPocketIMFC`, liegt fertig in
`core/src/fpga/core/cpu/VexPocketIMFC.v`, Quartus und Verilator brauchen also kein Scala.
Die Datei nicht von Hand ändern, sondern die Konfiguration, und neu erzeugen. Wer dabei das
Verhalten der CPU ändert, passt [`docs/soc.md`](../../docs/soc.md) und die Firmware an.

## Konfiguration

| Variante | Modul | Befehlssatz |
|---|---|---|
| `im` | `VexPocketIM` | RV32IM |
| `imf` | `VexPocketIMF` | RV32IM + F |
| `imfc` | `VexPocketIMFC` | RV32IM + F + eigene Befehle (CFU), **die CPU des Cores** |

`imf` ist dieselbe CPU ohne CFU, `im` zusätzlich ohne FPU; beide nur zum Vergleichen. Gemeinsam:

- 5-stufige Pipeline mit vollem Bypassing, Barrel-Shifter in einem Takt, Multiplizierer in
  DSP-Blöcken, Dividierer iterativ (34 Takte), statische Sprungvorhersage. Sprünge führt
  erst die Memory-Stufe aus (`earlyBranch = false`): In der Recherche schaffte die CPU mit
  FPU so 101–107 MHz, mit `earlyBranch = true` knapp 90 MHz.
- Je 8 KB I- und D-Cache, direkt abgebildet, 32-Byte-Zeilen, der D-Cache schreibt durch.
  Native Busse `iBus`/`dBus` direkt an `soc.v` (kein Wishbone), keine MMU, kein
  Debug-Modul, nur Machine-Mode. `reset` ist aktiv high und asynchron (wie in SpinalHDL
  üblich).
- Reset-Vektor `0x0000_0000`, `mtvec` nach Reset `0x0000_0020`, `0xF000_0000`–`0xFFFF_FFFF`
  ungecacht (Register, Framebuffer). Die Speicherkarte steht in `docs/soc.md`.
- Traps bei illegalen Befehlen, unausgerichteten Zugriffen, Busfehlern und `ecall`.
  `mcause`, `mepc` und `mtval` sind lesbar, damit der Trap-Handler einen Absturz ins
  Protokoll (`debug.txt`) schreiben kann. `mscratch` gibt es nicht, `ebreak` ist ein
  illegaler Befehl. `rdcycle` zählt Takte für Messungen.
- FPU (`imf`, `imfc`): RV32F mit Division und Wurzel, ohne `double`. Die Physik rechnet in
  `float` wie das Original ([`docs/architektur.md`](../../docs/architektur.md)).
- CFU (`imfc`): `custom-0` (Opcode `0x0B`), R-Format, `function_id = {funct7, funct3}`,
  zwei Registeroperanden, ein Ergebnis, ab Reset freigeschaltet. Die Rechenwerke hängen
  außerhalb der CPU am Bus `CfuPlugin_bus_*`, die Befehle stehen in `docs/soc.md`.

Belegung und Fmax der einzeln synthetisierten CPU: `docs/architektur.md`, „Geschätzte
Belegung“. Die CFU-Anbindung ändert daran kaum etwas (`imfc`: 3.000–3.037 ALMs, 24 M10K,
8 DSP, Fmax 105–108,5 MHz).

## Versionen

Alles ist festgelegt, damit jeder Lauf dieselbe Datei erzeugt:

| | |
|---|---|
| VexRiscv | Commit `baf7dc82f855eddaf5b0a20a6802c526f127e38a` (2026-09-24) |
| SpinalHDL, Scala, sbt | 1.13.0, 2.12.18, 1.6.0 (durch den VexRiscv-Commit festgelegt) |
| Docker-Image | `sbtscala/scala-sbt:eclipse-temurin-jammy-11.0.22_7_1.9.9_2.12.18` (JDK 11.0.22), Digest in `gen.sh` |

## Neu erzeugen

```sh
tools/vexriscv/gen.sh
```

Der erste Lauf klont VexRiscv nach `build/vexriscv/src` und lädt sbt, Scala und SpinalHDL
(rund 250 MB) nach `build/vexriscv/cache`, danach geht es ohne Netz. Alle Varianten landen
in `build/vexriscv/out/<variante>/`, `VexPocketIMFC.v` kopiert das Skript nach
`core/src/fpga/core/cpu/` und meldet, ob sie sich geändert hat. Dauer: 65 s beim ersten
Lauf, danach 34 s.

`sim/lint.vlt` schaltet Verilators Warnungen für die erzeugte Datei ab.

VexRiscv steht unter der MIT-Lizenz.

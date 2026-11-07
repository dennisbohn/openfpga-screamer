# VexRiscv

`PocketVex.scala` configures [VexRiscv](https://github.com/SpinalHDL/VexRiscv), and `gen.sh` generates
Verilog from it. The core's CPU, `VexPocketIMFC`, is checked in at `core/src/fpga/core/cpu/VexPocketIMFC.v`,
so Quartus and Verilator do not need Scala. Do not edit that file by hand: change the configuration and
regenerate. If the change alters the CPU's behaviour, update [`docs/soc.md`](../../docs/soc.md) and the
firmware too.

## Configuration

This section answers which CPU variants the configuration builds and what they have in common.

| Variant | Module | Instruction set |
|---|---|---|
| `im` | `VexPocketIM` | RV32IM |
| `imf` | `VexPocketIMF` | RV32IM + F |
| `imfc` | `VexPocketIMFC` | RV32IM + F + custom instructions (CFU), **the core's CPU** |

The variant `imf` is the same CPU without the CFU, and `im` is also without the FPU. Both exist for
comparison. All variants have these features in common:

- **Pipeline:** 5-stage pipeline with full bypassing, one-cycle barrel shifter, multiplier in DSP blocks,
  iterative divider (34 cycles), static branch prediction. Branches are resolved in the memory stage
  (`earlyBranch = false`). In the evaluation the CPU with FPU reached 101-107 MHz that way and just under
  90 MHz with `earlyBranch = true`.
- **Caches and buses:** 8 KB each of I- and D-cache, direct mapped, 32-byte lines, the D-cache writes
  through. The native buses `iBus`/`dBus` go straight to `soc.v` (no Wishbone). There is no MMU and no debug
  module, and the CPU runs in machine mode only. `reset` is active high and asynchronous (as usual in
  SpinalHDL).
- **Addresses:** Reset vector `0x0000_0000`, `mtvec` after reset `0x0000_0020`, `0xF000_0000`-`0xFFFF_FFFF`
  uncached (registers, framebuffer). The memory map is in `docs/soc.md`.
- **Traps:** The CPU traps on illegal instructions, misaligned accesses, bus errors and `ecall`. `mcause`,
  `mepc` and `mtval` are readable so that the trap handler can write a crash into the log (`debug.txt`).
  There is no `mscratch`, and `ebreak` is an illegal instruction. `rdcycle` counts clocks for measurements.

Two features exist only in some variants:

- **FPU** (`imf`, `imfc`): RV32F with divide and square root, no `double`. The physics computes in `float`
  like the original ([`docs/architecture.md`](../../docs/architecture.md)).
- **CFU** (`imfc`): `custom-0` (opcode `0x0B`), R format, `function_id = {funct7, funct3}`, two register
  operands, one result, enabled from reset. The arithmetic units sit outside the CPU on the bus
  `CfuPlugin_bus_*`. The instructions are listed in `docs/soc.md`.

The CPU `imfc` synthesized on its own needs 3,000-3,037 ALMs, 24 M10K and 8 DSP, with Fmax 105-108.5 MHz. The
CFU connection changes little about that.

## Versions

This section answers which versions the generator uses. Everything is pinned so that every run produces the
same file:

| | |
|---|---|
| VexRiscv | commit `baf7dc82f855eddaf5b0a20a6802c526f127e38a` (2026-09-24) |
| SpinalHDL, Scala, sbt | 1.13.0, 2.12.18, 1.6.0 (fixed by the VexRiscv commit) |
| Docker image | `sbtscala/scala-sbt:eclipse-temurin-jammy-11.0.22_7_1.9.9_2.12.18` (JDK 11.0.22), digest in `gen.sh` |

## Regenerating

This section answers how to generate the Verilog again.

```sh
tools/vexriscv/gen.sh
```

- The first run clones VexRiscv into `build/vexriscv/src` and downloads sbt, Scala and SpinalHDL (about
  250 MB) into `build/vexriscv/cache`. After that it works offline.
- All variants end up in `build/vexriscv/out/<variant>/`. The script copies `VexPocketIMFC.v` to
  `core/src/fpga/core/cpu/` and reports whether it changed.
- It takes 65 s the first time and 34 s after that.

`sim/lint.vlt` turns off Verilator's warnings for the generated file.

VexRiscv is licensed under the MIT license.

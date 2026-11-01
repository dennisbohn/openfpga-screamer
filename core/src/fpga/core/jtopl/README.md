# JTOPL (the FM chip)

The YM3812 / OPL2 of the AdLib, used for the game's MIDI music (`opl.v`, `firmware/fm.c`).
Written by Jose Tejada ([jotego/jtopl](https://github.com/jotego/jtopl)), not by us; the top
module is `jtopl2` (`jtopl` with `OPL_TYPE=2`, the OPL2 / YM3812: nine two-operator channels,
four wave shapes, rhythm mode).

| File | Origin | License |
|---|---|---|
| `jtopl*.v` | JTOPL commit `7ac0c81` with six changes, each marked `HYBRID:` in the source | GPL-3.0-or-later, see `LICENSE.JTOPL` |

The six changes come from `rtl/jtopl/` of the Hybrid MiSTer framework
([ItsDanik/Hybrid_MiSTer](https://github.com/ItsDanik/Hybrid_MiSTer), GPL-3.0), where they
were found by comparing the core with Nuked OPL3, which is taken from the chip's die. JTOPL
as it stands differs from the chip in these places: the feedback's strength (`jtopl_op.v`),
key scaling levels 1 and 2 swapped (`jtopl_eg_final.v`), rate scaling halved and attacks twice
too fast (`jtopl_eg_step.v`), the release's extra half step (`jtopl_eg_ctrl.v`), the four
fastest decays (`jtopl_eg_pure.v`) and a per-channel volume test its author left in
(`jtopl_acc.v`). Still slightly different from the chip, as noted there: attack rates 1 to 3,
the shape of the tremolo, and about 2 dB less of the upper harmonics of a note that is dying
away. Do not edit the files otherwise; when replacing them with a newer JTOPL, carry the six
changes over or check that each is no longer needed.

How the chip is clocked and fed, and what the CPU sees: `opl.v` and `docs/soc.md`.

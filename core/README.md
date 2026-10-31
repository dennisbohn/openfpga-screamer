# Core

The Pocket core, started from Analogue's [core template](https://github.com/open-fpga/core-template)
(v1.3.0).

| Path | Content |
|---|---|
| `core.json`, `data.json`, `input.json`, `interact.json`, `video.json`, `audio.json`, `variants.json` | the core definition: data slots, controls, Pocket menu, video and audio |
| `dist/` | files the SD card needs besides the core: icon, platform definition and image |
| `src/fpga/` | Quartus project (`ap_core.qpf`); `apf/` is Analogue's framework, `core/` is this project's hardware |
| `output/` | the bitstream, written by `tools/build.sh` |

## Legal

Analogue's Development program was created to further video game hardware preservation with
FPGA technology. Analogue does not support or endorse the unauthorized use or distribution of
material protected by copyright or other intellectual property rights.

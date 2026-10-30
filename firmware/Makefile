# The CPU's firmware (docs/soc.md): the boot loader for the boot RAM (firmware.bin, data
# slot 0) and the program for the SDRAM (program.bin, data slot 2). tools/fw.sh runs
# "make info" inside the toolchain image of firmware/docker/; "make -C firmware" works as
# well where riscv64-unknown-elf-gcc and picolibc are installed.
# Output in $(OUT): firmware.elf/.bin/.lst/.map and program.elf/.bin/.lst/.map
CROSS   ?= riscv64-unknown-elf-
OUT     ?= ../build/firmware$(if $(FAULT),-$(FAULT))
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
NM       = $(CROSS)nm
SIZE     = $(CROSS)size

# The CPU has no C (compressed) and no A (atomics) extension; picolibc only for memset/memcpy.
ARCH     = -march=rv32imf_zicsr_zifencei -mabi=ilp32f
CFLAGS   = $(ARCH) --specs=picolibc.specs -O2 -g -ffunction-sections -fdata-sections \
           -fno-math-errno -Wdouble-promotion -Wall -Wextra -MMD -MP
LDFLAGS  = $(ARCH) --specs=picolibc.specs -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments

# Size and CRC-32 of a file, in the words of the firmware's debug log
CRC32    = python3 -c 'import os, sys, zlib; d = open(sys.argv[1], "rb").read(); print(f"{os.path.basename(sys.argv[1])}: {len(d)} bytes, crc32 {zlib.crc32(d):08x}")'

# Fault tests for the simulation: make FAULT=IFETCH builds a boot loader that jumps out of
# the boot RAM after its self-tests (an instruction access fault, mcause 1). Each variant
# has its own OUT (../build/firmware-IFETCH), because the objects do not depend on CFLAGS.
ifneq ($(FAULT),)
CFLAGS  += -DFAULT_$(FAULT)
endif

BOOT_SRCS = start.S boot.c selftest.c console.c dlog.c trap.c memtest.c screen.c crc32.c slot.c
PROG_SRCS = start_prog.S main.c console.c dlog.c trap.c screen.c raster.c texdemo.c track.c crc32.c slot.c setup.c geom.c drive.c car.c opp.c ghost.c blit.c hud.c efx.c scenery.c sound.c speech.c music.c menu.c champ.c save.c intro.c \
            track_build.c dat.c tb_mmp.c tb_spl.c tb_gfx.c tb_hud.c
BOOT_OBJS = $(patsubst %,$(OUT)/obj/%.o,$(basename $(BOOT_SRCS)))
PROG_OBJS = $(patsubst %,$(OUT)/obj/%.o,$(basename $(PROG_SRCS)))

all: $(OUT)/firmware.bin $(OUT)/firmware.lst $(OUT)/program.bin $(OUT)/program.lst

# Section sizes, the RAM left for the stacks, and the CRC-32s that the debug log shows
info: all
	@$(SIZE) -A -x $(OUT)/firmware.elf | awk '$$2 ~ /^0x/ && $$2 != "0x0" && $$1 !~ /^\.(debug|riscv)/ && $$1 != "Total"'
	@end=$$($(NM) $(OUT)/firmware.elf | awk '$$3 == "__bss_end" { print $$1 }'); \
	 echo "boot RAM above the boot loader's .bss: $$((32768 - 0x$$end)) bytes, the program's stack above 24 KB"
	@$(CRC32) $(OUT)/firmware.bin
	@$(SIZE) -A -x $(OUT)/program.elf | awk '$$2 ~ /^0x/ && $$2 != "0x0" && $$1 !~ /^\.(debug|riscv)/ && $$1 != "Total"'
	@python3 -c 'import sys, zlib; d = open(sys.argv[1], "rb").read(); print(f"program.bin: {len(d)} bytes, crc32 {zlib.crc32(d[16:]):08x} (after the header)")' $(OUT)/program.bin

$(OUT)/obj/%.o: %.c Makefile | $(OUT)/obj
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.o: %.S Makefile | $(OUT)/obj
	@echo "  AS    $<"
	@$(CC) $(ARCH) -c $< -o $@

$(OUT)/firmware.elf: $(BOOT_OBJS) link.ld
	@echo "  LD    $(notdir $@)"
	@$(CC) $(LDFLAGS) -T link.ld -Wl,-Map=$(OUT)/firmware.map $(BOOT_OBJS) -o $@

$(OUT)/program.elf: $(PROG_OBJS) link_prog.ld
	@echo "  LD    $(notdir $@)"
	@$(CC) $(LDFLAGS) -T link_prog.ld -Wl,-Map=$(OUT)/program.map $(PROG_OBJS) -o $@

# The boot loader computes the CRC-32 of __image_start..__image_end: that range must be
# exactly this file. program.bin gets its CRC-32 into the header the boot loader checks.
$(OUT)/firmware.bin: $(OUT)/firmware.elf
	@$(OBJCOPY) -O binary $< $@
	@end=$$($(NM) $< | awk '$$3 == "__image_end" { print $$1 }'); size=$$(wc -c < $@); \
	 [ $$((0x$$end)) -eq $$size ] || { echo "__image_end 0x$$end is not the size of $@ ($$size)"; rm $@; exit 1; }

$(OUT)/program.bin: $(OUT)/program.elf prog_crc.py
	@$(OBJCOPY) -O binary $< $@
	@python3 prog_crc.py $@ || { rm $@; exit 1; }

$(OUT)/%.lst: $(OUT)/%.elf
	@$(OBJDUMP) -d $< > $@

$(OUT)/obj:
	@mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all info clean
-include $(BOOT_OBJS:.o=.d) $(PROG_OBJS:.o=.d)

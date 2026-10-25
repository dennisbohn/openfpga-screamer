# Step-1 firmware for the CPU (docs/soc.md). tools/fw.sh runs "make info" inside the
# toolchain image of firmware/docker/; "make -C firmware" works as well where
# riscv64-unknown-elf-gcc and picolibc are installed.
# Output in $(OUT): firmware.elf, .bin, .lst (objdump -d), .map
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
LDFLAGS  = $(ARCH) --specs=picolibc.specs -nostartfiles -T link.ld -Wl,--gc-sections \
           -Wl,--no-warn-rwx-segments -Wl,-Map=$(OUT)/firmware.map

# Size and CRC-32 of a file, in the words of the firmware's debug log
CRC32    = python3 -c 'import os, sys, zlib; d = open(sys.argv[1], "rb").read(); print(f"{os.path.basename(sys.argv[1])}: {len(d)} bytes, crc32 {zlib.crc32(d):08x}")'

# Fault tests for the simulation: make FAULT=IFETCH builds a firmware that jumps out of
# the boot RAM after its self-tests (an instruction access fault, mcause 1). Each variant
# has its own OUT (../build/firmware-IFETCH), because the objects do not depend on CFLAGS.
ifneq ($(FAULT),)
CFLAGS  += -DFAULT_$(FAULT)
endif

SRCS     = start.S main.c selftest.c console.c dlog.c trap.c
OBJS     = $(patsubst %,$(OUT)/obj/%.o,$(basename $(SRCS)))

all: $(OUT)/firmware.bin $(OUT)/firmware.lst

# Section sizes, the RAM left for the stack, and the CRC-32 that the firmware logs at boot
info: all
	@$(SIZE) -A -x $(OUT)/firmware.elf | awk '$$2 ~ /^0x/ && $$2 != "0x0" && $$1 !~ /^\.(debug|riscv)/ && $$1 != "Total"'
	@end=$$($(NM) $(OUT)/firmware.elf | awk '$$3 == "__bss_end" { print $$1 }'); \
	 echo "boot RAM above .bss (stack): $$((16384 - 0x$$end)) bytes"
	@$(CRC32) $(OUT)/firmware.bin

$(OUT)/obj/%.o: %.c Makefile | $(OUT)/obj
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.o: %.S Makefile | $(OUT)/obj
	@echo "  AS    $<"
	@$(CC) $(ARCH) -c $< -o $@

$(OUT)/firmware.elf: $(OBJS) link.ld
	@echo "  LD    $(notdir $@)"
	@$(CC) $(LDFLAGS) $(OBJS) -o $@

# The firmware computes the CRC-32 of __image_start..__image_end: that range must be
# exactly this file.
$(OUT)/firmware.bin: $(OUT)/firmware.elf
	@$(OBJCOPY) -O binary $< $@
	@end=$$($(NM) $< | awk '$$3 == "__image_end" { print $$1 }'); size=$$(wc -c < $@); \
	 [ $$((0x$$end)) -eq $$size ] || { echo "__image_end 0x$$end is not the size of $@ ($$size)"; rm $@; exit 1; }

$(OUT)/firmware.lst: $(OUT)/firmware.elf
	@$(OBJDUMP) -d $< > $@

$(OUT)/obj:
	@mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all info clean
-include $(OBJS:.o=.d)

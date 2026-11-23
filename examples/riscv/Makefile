# lastwords on RISC-V: QEMU virt, rv32imac, machine mode.
CROSS   ?= riscv-none-elf-
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size
QEMU    ?= qemu-system-riscv32
BUILD   := build
ARCH    := -march=rv32imac_zicsr -mabi=ilp32
CFLAGS  := $(ARCH) -std=c99 -Os -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
           -I../../src -DLW_RING_SIZE=2048
LDFLAGS := -T link.ld -nostartfiles -Wl,--no-warn-rwx-segments -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

.PHONY: all run size footprint count clean

all: $(BUILD)/demo-rv32.elf

$(BUILD)/demo-rv32.elf: main.c start.S ../../src/lastwords.c ../../src/lastwords.h link.ld
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD)/demo-rv32.map -o $@ start.S main.c ../../src/lastwords.c

run: $(BUILD)/demo-rv32.elf
	timeout 60 $(QEMU) -machine virt -bios none -nographic -monitor none -serial none \
	  -chardev file,id=out,path=$(BUILD)/run-rv32.log \
	  -semihosting-config enable=on,target=native,chardev=out -kernel $<
	python3 ../../tools/lwsym.py $< $(BUILD)/run-rv32.log > $(BUILD)/run-rv32.txt
	@cat $(BUILD)/run-rv32.txt
	python3 ../../tests/check_export.py $(BUILD)/run-rv32.log
	python3 ../../tests/check_riscv.py $(BUILD)/run-rv32.txt

size: all
	@$(SIZE) $(BUILD)/demo-rv32.elf

BENCH_FLAGS := $(ARCH) -std=c99 -Os -ffunction-sections -fdata-sections -I../../src -DLW_NO_PRINTF $(LDFLAGS)

# Flash that lastwords adds to an image that only calls lw_init and lw_log
# and installs the trap entry.
footprint:
	@mkdir -p $(BUILD)
	@$(CC) $(BENCH_FLAGS) -o $(BUILD)/base.elf bench-start.S bench.c
	@$(CC) $(BENCH_FLAGS) -DLW_BENCH_LINK -o $(BUILD)/with.elf bench-start.S bench.c ../../src/lastwords.c
	@b=$$($(SIZE) -A $(BUILD)/base.elf | awk '$$1==".text"{print $$2}'); \
	 w=$$($(SIZE) -A $(BUILD)/with.elf | awk '$$1==".text"{print $$2}'); \
	 echo "rv32imac: lastwords adds $$((w - b)) bytes of flash"
	@$(SIZE) -A $(BUILD)/with.elf | awk '$$1==".bss"||$$1==".sbss"||$$1==".noinit"{print "rv32imac: " $$1 " " $$2 " bytes"}'

# Instructions per lw_log() of a 32-byte line, from QEMU's execution trace.
count:
	@mkdir -p $(BUILD)
	@for n in 0 100; do \
	  $(CC) $(BENCH_FLAGS) -DN=$$n -o $(BUILD)/count$$n.elf bench-start.S bench.c ../../src/lastwords.c || exit 1; \
	  $(QEMU) -machine virt -bios none -nographic -monitor none -serial none \
	    -accel tcg,one-insn-per-tb=on -d exec,nochain -D $(BUILD)/count$$n.trace -kernel $(BUILD)/count$$n.elf; \
	done
	@a=$$(grep -c Trace $(BUILD)/count0.trace); b=$$(grep -c Trace $(BUILD)/count100.trace); \
	 echo "rv32imac: $$(( (b - a) / 100 )) instructions per lw_log() of 32 bytes"

clean:
	rm -rf $(BUILD)

# Cross-build the demo for four cores and run each in QEMU.
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size
QEMU    ?= qemu-system-arm
BUILD   := build
SRC     := main.c startup.c ../../src/lastwords.c
CFLAGS  := -std=c99 -Os -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
           -I../../src -DLW_RING_SIZE=2048
LDFLAGS := -T link.ld -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

# target  cpu            qemu machine
TARGETS := m0 m3 m4 m7
cpu_m0  := cortex-m0
cpu_m3  := cortex-m3
cpu_m4  := cortex-m4
cpu_m7  := cortex-m7
mach_m0 := microbit
mach_m3 := mps2-an385
mach_m4 := mps2-an386
mach_m7 := mps2-an500

.PHONY: all run-all size footprint count clean

all: $(addprefix $(BUILD)/demo-,$(addsuffix .elf,$(TARGETS)))

$(BUILD)/demo-%.elf: $(SRC) link.ld
	@mkdir -p $(BUILD)
	$(CC) -mcpu=$(cpu_$*) -mthumb $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD)/demo-$*.map -o $@ $(SRC)

run-%: $(BUILD)/demo-%.elf
	timeout 60 $(QEMU) -machine $(mach_$*) -nographic -monitor none -serial none \
	  -chardev file,id=out,path=$(BUILD)/run-$*.log \
	  -semihosting-config enable=on,target=native,chardev=out -kernel $<
	python3 ../../tools/lwsym.py $< $(BUILD)/run-$*.log > $(BUILD)/run-$*.txt
	@cat $(BUILD)/run-$*.txt
	python3 ../../tests/check_export.py $(BUILD)/run-$*.log

run-all: $(addprefix run-,$(TARGETS))
	python3 ../../tests/check_qemu.py $(BUILD)/run-*.txt

size: all
	@$(SIZE) $(BUILD)/*.elf

BENCH_FLAGS := -mcpu=cortex-m3 -mthumb -std=c99 -Os -ffunction-sections -fdata-sections -I../../src \
               -DLW_NO_PRINTF $(LDFLAGS)

# Flash that lastwords adds to an image that only calls lw_init and lw_log.
footprint:
	@mkdir -p $(BUILD)
	@$(CC) $(BENCH_FLAGS) -o $(BUILD)/base.elf bench.c startup.c -DLW_NO_FAULT_HANDLERS
	@$(CC) $(BENCH_FLAGS) -DLW_BENCH_LINK -o $(BUILD)/with.elf bench.c startup.c ../../src/lastwords.c
	@b=$$($(SIZE) -A $(BUILD)/base.elf | awk '$$1==".text"{print $$2}'); \
	 w=$$($(SIZE) -A $(BUILD)/with.elf | awk '$$1==".text"{print $$2}'); \
	 echo "cortex-m3: lastwords adds $$((w - b)) bytes of flash"

# Instructions per lw_log() of a 32-byte line, from QEMU's execution trace.
count:
	@mkdir -p $(BUILD)
	@for n in 0 100; do \
	  $(CC) $(BENCH_FLAGS) -DN=$$n -o $(BUILD)/count$$n.elf bench.c startup.c ../../src/lastwords.c || exit 1; \
	  $(QEMU) -machine mps2-an385 -nographic -monitor none -serial none -semihosting-config enable=on,target=native \
	    -accel tcg,one-insn-per-tb=on -d exec,nochain -D $(BUILD)/count$$n.trace -kernel $(BUILD)/count$$n.elf; \
	done
	@a=$$(grep -c Trace $(BUILD)/count0.trace); b=$$(grep -c Trace $(BUILD)/count100.trace); \
	 echo "cortex-m3: $$(( (b - a) / 100 )) instructions per lw_log() of 32 bytes"

clean:
	rm -rf $(BUILD)

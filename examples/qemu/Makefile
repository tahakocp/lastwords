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

.PHONY: all run-all size clean

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

run-all: $(addprefix run-,$(TARGETS))
	python3 ../../tests/check_qemu.py $(BUILD)/run-*.txt

size: all
	@$(SIZE) $(BUILD)/*.elf

clean:
	rm -rf $(BUILD)

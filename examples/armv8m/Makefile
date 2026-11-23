# lastwords on a TrustZone Cortex-M55: QEMU mps3-an547, Secure image.
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size
QEMU    ?= qemu-system-arm
BUILD   := build
CFLAGS  := -mcpu=cortex-m55 -mthumb -mcmse -std=c99 -Os -g -Wall -Wextra -Werror \
           -ffunction-sections -fdata-sections -I../../src -DLW_RING_SIZE=2048
LDFLAGS := -T link.ld -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

.PHONY: all run size clean

all: $(BUILD)/demo-m55.elf

$(BUILD)/demo-m55.elf: main.c startup.c ../../src/lastwords.c ../../src/lastwords.h link.ld
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD)/demo-m55.map -o $@ main.c startup.c ../../src/lastwords.c

run: $(BUILD)/demo-m55.elf
	timeout 60 $(QEMU) -machine mps3-an547 -nographic -monitor none -serial none \
	  -chardev file,id=out,path=$(BUILD)/run-m55.log \
	  -semihosting-config enable=on,target=native,chardev=out -kernel $<
	python3 ../../tools/lwsym.py $< $(BUILD)/run-m55.log > $(BUILD)/run-m55.txt
	@cat $(BUILD)/run-m55.txt
	python3 ../../tests/check_export.py $(BUILD)/run-m55.log
	python3 ../../tests/check_armv8m.py $(BUILD)/run-m55.txt

size: all
	@$(SIZE) $(BUILD)/demo-m55.elf

clean:
	rm -rf $(BUILD)

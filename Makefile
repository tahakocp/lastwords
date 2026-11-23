CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror
BUILD   := build

.PHONY: test test-asan qemu riscv armv8m size footprint count cross clean

CROSS_CPUS := cortex-m0plus cortex-m3 cortex-m4 cortex-m7 cortex-m23 cortex-m33 cortex-m55

test:
	@mkdir -p $(BUILD)
	@for n in 256 1024 4096; do \
	  $(CC) $(CFLAGS) -DLW_TEST_HOOK -DLW_RING_SIZE=$$n -Isrc -o $(BUILD)/test_$$n tests/test_host.c src/lastwords.c || exit 1; \
	  $(BUILD)/test_$$n || exit 1; \
	done
	@$(CC) $(CFLAGS) -DLW_TEST_HOOK -Isrc -o $(BUILD)/test_crash tests/test_crash.c src/lastwords.c
	@$(BUILD)/test_crash
	@$(CC) $(CFLAGS) -DLW_TEST_HOOK -DLW_NO_DEDUP -Isrc -o $(BUILD)/test_crash_nodedup tests/test_crash.c src/lastwords.c
	@$(BUILD)/test_crash_nodedup | head -1
	@python3 tests/test_decode.py $(BUILD)

test-asan:
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover -DLW_TEST_HOOK -Isrc \
	  -o $(BUILD)/test_asan tests/test_host.c src/lastwords.c
	$(BUILD)/test_asan 2000
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover -DLW_TEST_HOOK -Isrc \
	  -o $(BUILD)/test_crash_asan tests/test_crash.c src/lastwords.c
	$(BUILD)/test_crash_asan

qemu:
	$(MAKE) -C examples/qemu run-all

# RV32 machine-mode trap capture on QEMU virt (needs riscv-none-elf-gcc).
riscv:
	$(MAKE) -C examples/riscv run
	@$(MAKE) -s -C examples/riscv footprint count

# TrustZone Cortex-M55 on QEMU mps3-an547, Secure image with a Non-secure call.
armv8m:
	$(MAKE) -C examples/armv8m run

# Compile-only check for cores QEMU's free machines do not cover.
cross:
	@for c in $(CROSS_CPUS); do \
	  arm-none-eabi-gcc -mcpu=$$c -mthumb -Os -std=c99 -Wall -Wextra -Wpedantic -Werror \
	    -c src/lastwords.c -o /dev/null || exit 1; echo "$$c ok"; \
	done

size:
	$(MAKE) -C examples/qemu size

footprint count:
	@$(MAKE) -s -C examples/qemu $@

clean:
	rm -rf $(BUILD)
	$(MAKE) -C examples/qemu clean
	$(MAKE) -C examples/riscv clean
	$(MAKE) -C examples/armv8m clean

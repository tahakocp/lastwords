CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror
BUILD   := build

.PHONY: test test-asan qemu size cross clean

CROSS_CPUS := cortex-m0plus cortex-m3 cortex-m4 cortex-m7 cortex-m23 cortex-m33 cortex-m55

test:
	@mkdir -p $(BUILD)
	@for n in 256 1024 4096; do \
	  $(CC) $(CFLAGS) -DLW_TEST_HOOK -DLW_RING_SIZE=$$n -Isrc -o $(BUILD)/test_$$n tests/test_host.c src/lastwords.c || exit 1; \
	  $(BUILD)/test_$$n || exit 1; \
	done

test-asan:
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover -DLW_TEST_HOOK -Isrc \
	  -o $(BUILD)/test_asan tests/test_host.c src/lastwords.c
	$(BUILD)/test_asan 2000

qemu:
	$(MAKE) -C examples/qemu run-all

# Compile-only check for cores QEMU's free machines do not cover.
cross:
	@for c in $(CROSS_CPUS); do \
	  arm-none-eabi-gcc -mcpu=$$c -mthumb -Os -std=c99 -Wall -Wextra -Wpedantic -Werror \
	    -c src/lastwords.c -o /dev/null || exit 1; echo "$$c ok"; \
	done

size:
	$(MAKE) -C examples/qemu size

clean:
	rm -rf $(BUILD)
	$(MAKE) -C examples/qemu clean

# Host tests for the M1OS app runtime. Runs the REAL firmware sources against
# the stub layer in host_stubs/ (RAM FatFs, FreeRTOS, logging) under
# ASan+UBSan. From the repo root:  make -C m1_csrc/test
#
# Targets:
#   test          run all host tests (default)
#   test-storage  storage umbrella + path policy
#   test-elf      ELF loader robustness suite
#   fuzz-elf      build+run the libFuzzer harness briefly (clang only)

# Default to `clang` (Homebrew LLVM on dev Macs): unlike Apple's /usr/bin/clang
# it ships LeakSanitizer for darwin, so local runs leak-gate exactly like the
# Linux CI fuzz job. cc/Accepted clang still work via CC= override.
CC      ?= clang
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
INCLUDES = -Ihost_stubs -I..
STUB_SRC = host_stubs/host_stubs.c

STO_SRC = ../m1_app_storage.c
ELF_SRC = ../m1_elf_loader.c

.PHONY: test test-storage test-elf test-ui test-lease test-crash test-radio fuzz-elf clean

test: test-storage test-elf test-ui test-lease test-crash test-radio

test-storage: test_m1_app_storage.c $(STO_SRC) host_stubs/ff.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_app_storage.c $(STO_SRC) $(STUB_SRC)
	./$@

test-elf: test_m1_elf_loader.c $(ELF_SRC) host_stubs/ff.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_elf_loader.c $(ELF_SRC) $(STUB_SRC)
	./$@

test-ui: test_m1_ui.c ../m1_app_ui.c ../m1_ui_focus.c host_stubs/ff.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_ui.c ../m1_app_ui.c ../m1_ui_focus.c $(STUB_SRC)
	./$@

test-lease: test_m1_esp_lease.c ../m1_esp_lease.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_esp_lease.c ../m1_esp_lease.c $(STUB_SRC)
	./$@

test-crash: test_m1_crash.c ../m1_crash_core.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_crash.c ../m1_crash_core.c $(STUB_SRC)
	./$@

test-radio: test_m1_radio_policy.c ../m1_radio_policy.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_m1_radio_policy.c ../m1_radio_policy.c $(STUB_SRC)
	./$@

fuzz-elf: fuzz_m1_elf_loader.c $(ELF_SRC) host_stubs/ff.h
	@command -v clang >/dev/null 2>&1 || { echo "clang not found (libFuzzer needs it)"; exit 1; }
	clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined $(INCLUDES) \
	      -o $@ fuzz_m1_elf_loader.c $(ELF_SRC) $(STUB_SRC)
	./$@ $(FUZZ_ARGS)
FUZZ_ARGS ?= -max_total_time=15 -max_len=8192

clean:
	rm -f test-storage test-elf test-ui test-lease test-crash fuzz-elf

# json-lib build, test, distribution and benchmark workflow.
#
# This Makefile follows the directory and target conventions of bignum-template.
# It intentionally uses only standard make variables and shell commands so that
# all artifacts remain inspectable and reproducible in a C11 toolchain.

# -----------------------------------------------------------------------------
# User-configurable execution parameters.
# -----------------------------------------------------------------------------

CONFIG ?= debug
REPORT_NAME ?= current
SAN ?= address
VALGRIND ?= valgrind
PERF ?= /usr/local/bin/perf
PERF_RUNS ?= 5
PERF_EVENTS ?= task-clock,context-switches,cpu-migrations,page-faults
JSON_BENCH_DOCUMENTS ?= 10000
JSON_BENCH_DEPTH ?= 8

# -----------------------------------------------------------------------------
# Repository-derived names and tool commands.
# -----------------------------------------------------------------------------

REPOSITORY_NAME := $(notdir $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST))))))
LIB_NAME := $(subst -,_,$(REPOSITORY_NAME))
UPPER_LIB_NAME := JSON_LIB

CC = gcc
AR = ar
RL = ranlib
CPPCHECK = cppcheck
RM = rm -rf
MKDIR = mkdir -p

# -----------------------------------------------------------------------------
# Project layout and generated artifact paths.
# -----------------------------------------------------------------------------

SRC_DIR = src
INCLUDE_DIR = include
TESTS_DIR = tests
BENCH_DIR = benchmarks
REPORTS_DIR = $(BENCH_DIR)/reports
BUILD_DIR = build
BIN_DIR = bin
DIST_DIR = dist
DIST_INCLUDE_DIR = $(DIST_DIR)/include
DIST_LIB_DIR = $(DIST_DIR)/lib

HEADER = $(INCLUDE_DIR)/$(LIB_NAME).h
SOURCE = $(SRC_DIR)/$(LIB_NAME).c
OBJ = $(BUILD_DIR)/$(LIB_NAME).o
STATIC_LIB = $(DIST_LIB_DIR)/lib$(LIB_NAME).a
SINGLE_HEADER = $(DIST_INCLUDE_DIR)/$(LIB_NAME).h
TEST_SRCS := $(wildcard $(TESTS_DIR)/*.c)
TEST_BINS := $(patsubst $(TESTS_DIR)/%.c,$(BIN_DIR)/%,$(TEST_SRCS))
TEST_BINS_MT := $(filter $(BIN_DIR)/%_mt,$(TEST_BINS))
BENCH_SRC = $(BENCH_DIR)/bench_$(LIB_NAME).c
BENCH_BIN = $(BIN_DIR)/bench_$(LIB_NAME)
BENCH_RUNTIME = $(REPORTS_DIR)/$(REPORT_NAME)_runtime.txt
BENCH_DATA = $(REPORTS_DIR)/$(REPORT_NAME).perf.data
BENCH_REPORT = $(REPORTS_DIR)/$(REPORT_NAME).txt
BENCH_STAT = $(REPORTS_DIR)/$(REPORT_NAME)_stat.csv

# -----------------------------------------------------------------------------
# Strict C11 compiler and linker configuration.
# -----------------------------------------------------------------------------

CFLAGS_BASE = -std=c11 -Wall -Wextra -Werror -pedantic -I$(INCLUDE_DIR)
LDFLAGS_BASE = -lm

ifeq ($(CONFIG),release)
CFLAGS_CONFIG = -O2 -march=x86-64
else
CFLAGS_CONFIG = -O1 -g
endif

ifeq ($(SAN),address)
SAN_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS = -fsanitize=address,undefined
SAN_LABEL = AddressSanitizer and UBSan
else ifeq ($(SAN),undefined)
SAN_CFLAGS = -fsanitize=undefined -fno-omit-frame-pointer
SAN_LDFLAGS = -fsanitize=undefined
SAN_LABEL = UndefinedBehaviorSanitizer
else
SAN_CFLAGS =
SAN_LDFLAGS =
SAN_LABEL = none
endif

CFLAGS = $(CFLAGS_BASE) $(CFLAGS_CONFIG) $(SAN_CFLAGS)
LDFLAGS = $(LDFLAGS_BASE) $(SAN_LDFLAGS)
HELGRIND_CFLAGS = $(CFLAGS_BASE) -O1 -g -fno-omit-frame-pointer -march=x86-64

.DEFAULT_GOAL := all
.PHONY: all build lint test test_sanitize test_helgrind bench bench_stat install dist clean help

# Build the library object with the selected CONFIG/SAN variables.
all: build

build: $(OBJ)

$(OBJ): $(SOURCE) $(HEADER) | $(BUILD_DIR)/.dir
	$(CC) $(CFLAGS) -c $< -o $@

# Build every tests/<name>.c executable against the current library object.
$(BIN_DIR)/%: $(TESTS_DIR)/%.c $(OBJ) $(HEADER) | $(BIN_DIR)/.dir
	$(CC) $(CFLAGS) $< $(OBJ) -o $@ $(LDFLAGS) $(if $(filter %_mt,$*),-pthread)

# Build the benchmark binary with performance flags and no sanitizer link set.
$(BENCH_BIN): $(BENCH_SRC) $(OBJ) $(HEADER) | $(BIN_DIR)/.dir
	$(CC) $(CFLAGS_BASE) -O2 -march=x86-64 $< $(OBJ) -o $@ $(LDFLAGS_BASE)

# Create tracked directory sentinels without adding generated artifacts to Git.
$(BUILD_DIR)/.dir $(BIN_DIR)/.dir $(REPORTS_DIR)/.dir $(DIST_INCLUDE_DIR)/.dir $(DIST_LIB_DIR)/.dir:
	$(MKDIR) $(@D)
	@touch $@

# Analyse every C source file in src/, tests/ and benchmarks/ with cppcheck.
lint:
	$(CPPCHECK) --std=c11 --enable=all --error-exitcode=1 \
		--suppress=missingIncludeSystem --inline-suppr --inconclusive \
		--check-config -I$(INCLUDE_DIR) $(SRC_DIR) $(TESTS_DIR) $(BENCH_DIR)

# Run every discovered test binary and preserve the template summary protocol.
test: $(TEST_BINS)
	@total=0; failed=0; \
	for test_binary in $(TEST_BINS); do \
		total=$$((total + 1)); echo "--- $$test_binary ---"; \
		if ./$$test_binary; then :; else failed=$$((failed + 1)); fi; \
	done; \
	echo "=== Summary: $$failed / $$total failed ==="; test $$failed -eq 0

# Rebuild the real test suite from clean state with the selected sanitizer set.
test_sanitize:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test CONFIG=debug SAN=$(SAN)
	@echo "$(SAN_LABEL): OK"

# Run ordinary tests, then apply Helgrind to all future *_mt test executables.
test_helgrind:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test CFLAGS="$(HELGRIND_CFLAGS)" SAN=no
	@total=0; failed=0; \
	for test_binary in $(TEST_BINS_MT); do \
		total=$$((total + 1)); log=$(BIN_DIR)/helgrind_$$(basename $$test_binary).log; \
		if $(VALGRIND) --tool=helgrind --error-exitcode=42 --log-file=$$log \
			./$$test_binary > /dev/null 2>&1; then :; else failed=$$((failed + 1)); fi; \
	done; \
	echo "=== Helgrind: $$failed / $$total failed ==="; test $$failed -eq 0

# Benchmarks must rebuild their prerequisites without sanitizer instrumentation.
# Target-specific CFLAGS/LDFLAGS prevent reuse of ASan/UBSan library objects.
bench bench_stat: SAN=no
bench bench_stat: CFLAGS=$(CFLAGS_BASE) $(CFLAGS_CONFIG)
bench bench_stat: LDFLAGS=$(LDFLAGS_BASE)

# Record a sampling profile and verify the final benchmark completion marker.
bench: $(BENCH_BIN) | $(REPORTS_DIR)/.dir
	@$(PERF) record -o $(BENCH_DATA) -F 1000 -g -- $(BENCH_BIN) \
		--documents $(JSON_BENCH_DOCUMENTS) --depth $(JSON_BENCH_DEPTH) \
		> $(BENCH_RUNTIME) 2>&1
	@test "$$(grep -c '^Benchmark finished[.]$$' $(BENCH_RUNTIME))" -eq 1
	@$(PERF) report -i $(BENCH_DATA) --stdio > $(BENCH_REPORT)
	@echo "Benchmark report: $(BENCH_REPORT)"

# Collect repeated software-counter measurements and validate every run marker.
bench_stat: $(BENCH_BIN) | $(REPORTS_DIR)/.dir
	@$(PERF) stat -r $(PERF_RUNS) -x, -e $(PERF_EVENTS) -o $(BENCH_STAT) -- \
		$(BENCH_BIN) --documents $(JSON_BENCH_DOCUMENTS) --depth $(JSON_BENCH_DEPTH) \
		> $(BENCH_RUNTIME) 2>&1
	@test "$$(grep -c '^Benchmark finished[.]$$' $(BENCH_RUNTIME))" -eq $(PERF_RUNS)
	@echo "Benchmark statistics: $(BENCH_STAT)"

# Install the public header and archive without copying generated test artifacts.
install: build | $(DIST_INCLUDE_DIR)/.dir $(DIST_LIB_DIR)/.dir
	cp $(HEADER) $(SINGLE_HEADER)
	$(AR) rcs $(STATIC_LIB) $(OBJ)
	$(RL) $(STATIC_LIB)

# Create a complete redistributable package from a clean build state.
dist: clean install
	cp README.md LICENSE $(DIST_DIR)/

# Delete all reproducibly generated C build and distribution artifacts.
clean:
	$(RM) $(BUILD_DIR) $(BIN_DIR) $(DIST_DIR)

# Describe targets and documented user-configurable benchmark variables.
help:
	@echo "Usage: make <target> [CONFIG=release] [REPORT_NAME=name]"
	@echo "Main targets: all/build, lint, test, test_sanitize, test_helgrind"
	@echo "              bench, bench_stat, install, dist, clean, help"
	@echo "Benchmark variables: JSON_BENCH_DOCUMENTS=<positive integer>"
	@echo "                     JSON_BENCH_DEPTH=<positive integer>"
	@echo "                     PERF_RUNS=<count> PERF_EVENTS=<events>"

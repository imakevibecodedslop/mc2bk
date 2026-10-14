# builds the mouse companion mod: the mips mod code (build/mod.elf, then
# RecompModTool makes the .nrm) and the native library (build/mc_mouse_native.so).
# run from this folder: `make`, then `../RecompModTool mod.toml build`.

BUILD_DIR := build
ROOT := ..

CC      := clang
LD      := ld.lld
HOSTCC  ?= gcc

TARGET  := $(BUILD_DIR)/mod.elf
NATIVE  := $(BUILD_DIR)/mc_mouse_native.so

LDSCRIPT := $(ROOT)/mod.ld
ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset -funsigned-char
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-unsupported-floating-point-opt -Werror=section -Wno-visibility
CFLAGS   := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS := -nostdinc -DMIPS -I $(ROOT)/include
LDFLAGS  := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic -gc-sections

NATIVE_CFLAGS := -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra

all: $(TARGET) $(NATIVE)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/mouse_mod.o: src/mouse_mod.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@

$(TARGET): $(BUILD_DIR)/mouse_mod.o $(LDSCRIPT)
	$(LD) $< $(LDFLAGS) -o $@

$(NATIVE): native/mc_mouse_native.c | $(BUILD_DIR)
	$(HOSTCC) $(NATIVE_CFLAGS) $< -o $@ -ldl -lm -lz

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean

# builds the skip intro mod. from this folder: `make`, then `RecompModTool mod.toml build`.

BUILD_DIR := build
ROOT := ..

CC := clang
LD := ld.lld

TARGET := $(BUILD_DIR)/mod.elf
LDSCRIPT := $(ROOT)/mod.ld
ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset -funsigned-char
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-unsupported-floating-point-opt -Werror=section -Wno-visibility
CFLAGS   := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS := -nostdinc -DMIPS -I $(ROOT)/include -I $(ROOT)/bk-decomp/include/2.0L -I $(ROOT)/bk-decomp/include/2.0L/PR
LDFLAGS  := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic -gc-sections

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/skip_intro.o: src/skip_intro.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@

$(TARGET): $(BUILD_DIR)/skip_intro.o $(LDSCRIPT)
	$(LD) $< $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean

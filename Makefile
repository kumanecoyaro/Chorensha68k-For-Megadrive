# Chorensha68k Mega Drive port
#
#   make XDF=path/to/CH68_110_1.xdf     -> out/chorensha68k_md.bin
#
# Needs: m68k-linux-gnu-gcc / binutils (Debian/Ubuntu: gcc-m68k-linux-gnu), python3 + Pillow.

XDF    ?= CH68_110_1.xdf
PREFIX ?= m68k-linux-gnu-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
PY      = python3

BUILD = build
GEN   = $(BUILD)/gen
OUT   = out
ROM   = $(OUT)/chorensha68k_md.bin

CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -fno-pic \
         -fno-asynchronous-unwind-tables -Wall -Wno-main -Isrc -I$(GEN)
LDFLAGS = -m68000 -nostdlib -static -Wl,--build-id=none -T src/md.ld

SRCS = src/main.c src/vdp.c src/sprite.c src/tables.c src/bg.c src/obj.c \
       src/player.c src/enemy.c src/stage.c src/sound.c $(GEN)/assets.c $(GEN)/pcm.c $(GEN)/z80drv.c
OBJS = $(BUILD)/crt0.o $(BUILD)/pcm_data.o $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

all: $(ROM)

# 1. unpack the disk image (Human68k FAT12) and the game's asset archive
$(BUILD)/disk/CRS68K/SZ2.X: $(XDF) tools/xdf_extract.py
	$(PY) tools/xdf_extract.py $(XDF) $(BUILD)/disk > /dev/null

$(BUILD)/assets/XSP_DAT/map.bg0: $(BUILD)/disk/CRS68K/SZ2.X
	mkdir -p $(BUILD)/assets && tar xzf $(BUILD)/disk/CRS68K/assets.tgz -C $(BUILD)/assets

# 2. convert graphics / maps / tables
$(GEN)/assets.c $(GEN)/assets.h: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_assets.py tools/x68gfx.py
	$(PY) tools/convert_assets.py $(BUILD)/disk/CRS68K/SZ2.X $(BUILD)/assets/XSP_DAT $(GEN)

$(GEN)/pcm.c $(GEN)/pcm.h $(GEN)/pcm.bin: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_pcm.py
	$(PY) tools/convert_pcm.py $(BUILD)/disk/CRS68K/PCM_DAT/SZ2_PCM.CNF $(BUILD)/assets/PCM_DAT $(GEN)

$(GEN)/z80drv.c: tools/z80_pcm_driver.py
	mkdir -p $(GEN)
	$(PY) tools/z80_pcm_driver.py $@

# 3. compile
$(BUILD)/crt0.o: src/crt0.s
	mkdir -p $(BUILD)
	$(CC) -m68000 -c $< -o $@

$(BUILD)/pcm_data.o: src/pcm_data.s $(GEN)/pcm.bin
	$(CC) -m68000 -Wa,-I$(GEN) -c $< -o $@

$(BUILD)/%.o: src/%.c src/*.h $(GEN)/assets.h $(GEN)/pcm.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(GEN)/%.c $(GEN)/assets.h $(GEN)/pcm.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/game.elf: $(OBJS) src/md.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(ROM): $(BUILD)/game.elf tools/fixrom.py
	mkdir -p $(OUT)
	$(OBJCOPY) -O binary $< $(BUILD)/game.raw
	$(PY) tools/fixrom.py $(BUILD)/game.raw $@
	$(PY) tools/rom_budget.py $(BUILD) $@

# disassembly listing of the original executable (analysis aid)
disasm: $(BUILD)/disk/CRS68K/SZ2.X
	$(PY) tools/dis68.py $< $(BUILD)/SZ2.lst

clean:
	rm -rf $(BUILD)

.PHONY: all clean disasm

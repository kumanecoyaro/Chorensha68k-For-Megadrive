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

CFLAGS = -m68000 -mstrict-align -O2 -fno-strict-aliasing -fomit-frame-pointer -ffreestanding -fno-builtin -fno-pic \
         -fno-asynchronous-unwind-tables -Wall -Wno-main -Isrc -I$(GEN)
LDFLAGS = -m68000 -nostdlib -static -Wl,--build-id=none -T src/md.ld

SRCS = src/main.c src/x68.c src/xsp.c src/vdp.c src/bg.c src/plane.c src/title.c src/sound.c src/music.c \
       $(GEN)/assets.c $(GEN)/pcm.c $(GEN)/z80drv.c $(GEN)/bgm.c $(GEN)/spr.c
OBJS = $(BUILD)/crt0.o $(BUILD)/hooks.o $(BUILD)/xsp_fast.o $(BUILD)/pcm_data.o $(BUILD)/spr_data.o $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

all: $(ROM)

# 1. unpack the disk image (Human68k FAT12) and the game's asset archive
$(BUILD)/disk/CRS68K/SZ2.X: $(XDF) tools/xdf_extract.py
	$(PY) tools/xdf_extract.py $(XDF) $(BUILD)/disk > /dev/null

$(BUILD)/assets/XSP_DAT/map.bg0: $(BUILD)/disk/CRS68K/SZ2.X
	mkdir -p $(BUILD)/assets && tar xzf $(BUILD)/disk/CRS68K/assets.tgz -C $(BUILD)/assets

# 2. convert graphics / maps / tables
$(GEN)/assets.c $(GEN)/assets.h: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_assets.py tools/x68gfx.py
	$(PY) tools/convert_assets.py $(BUILD)/disk/CRS68K/SZ2.X $(BUILD)/assets/XSP_DAT $(GEN)

$(GEN)/spr.c $(GEN)/spr.h $(GEN)/spr_tiles.bin $(GEN)/spr_def.inc: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_sprites.py tools/x68gfx.py tools/hot_keys.txt
	$(PY) tools/convert_sprites.py $(BUILD)/assets/XSP_DAT $(GEN)

$(GEN)/pcm.c $(GEN)/pcm.h $(GEN)/pcm.bin: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_pcm.py
	$(PY) tools/convert_pcm.py $(BUILD)/disk/CRS68K/PCM_DAT/SZ2_PCM.CNF $(BUILD)/assets/PCM_DAT $(GEN)

$(GEN)/bgm.c $(GEN)/bgm.h: $(BUILD)/assets/XSP_DAT/map.bg0 tools/convert_bgm.py tools/mdx.py
	$(PY) tools/convert_bgm.py $(BUILD)/disk/CRS68K/BGM_DAT/SZ2_BGM.CNF $(BUILD)/disk/CRS68K/BGM_DAT/original $(GEN)

$(GEN)/z80drv.c: tools/z80_pcm_driver.py
	mkdir -p $(GEN)
	$(PY) tools/z80_pcm_driver.py $@

# relocated original program (game logic runs as is; see tools/relocate.py)
$(BUILD)/SZ2.lst: $(BUILD)/disk/CRS68K/SZ2.X tools/dis68.py
	$(PY) tools/dis68.py $< $@

$(GEN)/orig.bin $(GEN)/orig_ram.bin $(GEN)/orig.h $(GEN)/hooks.inc: $(BUILD)/SZ2.lst $(BUILD)/assets/XSP_DAT/map.bg0 tools/relocate.py tools/xfile.py
	mkdir -p $(GEN)
	$(PY) tools/relocate.py $(BUILD)/disk/CRS68K/SZ2.X $(BUILD)/SZ2.lst $(BUILD)/assets/XSP_DAT $(GEN)

# 3. compile
$(BUILD)/crt0.o: src/crt0.s
	mkdir -p $(BUILD)
	$(CC) -m68000 -c $< -o $@

$(BUILD)/hooks.o: src/hooks.s $(GEN)/hooks.inc $(GEN)/orig.bin $(GEN)/orig_ram.bin
	$(CC) -m68000 -Wa,-I$(GEN) -c $< -o $@

$(BUILD)/xsp_fast.o: src/xsp_fast.s $(GEN)/spr_def.inc
	$(CC) -m68000 -Wa,-I$(GEN) -c $< -o $@

$(BUILD)/spr_data.o: src/spr_data.s $(GEN)/spr_tiles.bin
	$(CC) -m68000 -Wa,-I$(GEN) -c $< -o $@

$(BUILD)/pcm_data.o: src/pcm_data.s $(GEN)/pcm.bin
	$(CC) -m68000 -Wa,-I$(GEN) -c $< -o $@

$(BUILD)/%.o: src/%.c src/*.h $(GEN)/assets.h $(GEN)/pcm.h $(GEN)/bgm.h $(GEN)/orig.h $(GEN)/spr.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(GEN)/%.c $(GEN)/assets.h $(GEN)/pcm.h $(GEN)/bgm.h $(GEN)/spr.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/game.elf: $(OBJS) src/md.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(ROM): $(BUILD)/game.elf tools/fixrom.py
	mkdir -p $(OUT)
	$(OBJCOPY) -O binary $< $(BUILD)/game.raw
	$(PY) tools/fixrom.py $(BUILD)/game.raw $@
	$(PY) tools/rom_budget.py $(BUILD) $@

# disassembly listing of the original executable (analysis aid)
disasm: $(BUILD)/SZ2.lst

clean:
	rm -rf $(BUILD)

.PHONY: all clean disasm

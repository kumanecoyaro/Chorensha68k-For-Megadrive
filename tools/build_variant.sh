#!/bin/sh
# Debug build of the ROM with extra defines: build_variant.sh NAME "-DFOO -DBAR=1"
# (expects a normal `make` to have run first; output build/var_NAME/NAME.bin)
set -e
name=${1:?name}
d="build/var_${name}"
mkdir -p "$d"
rm -f "${d:?}"/*.o
for f in src/*.c build/gen/*.c; do
    m68k-linux-gnu-gcc -m68000 -mstrict-align -O2 -fomit-frame-pointer -ffreestanding -fno-builtin \
        -fno-pic -fno-asynchronous-unwind-tables -Isrc -Ibuild/gen $2 -c "$f" -o "$d/$(basename "$f" .c).o"
done
m68k-linux-gnu-gcc -m68000 -nostdlib -static -Wl,--build-id=none -T src/md.ld build/crt0.o build/pcm_data.o \
    "$d"/*.o -o "$d/g.elf"
m68k-linux-gnu-objcopy -O binary "$d/g.elf" "$d/g.raw"
python3 tools/fixrom.py "$d/g.raw" "$d/$name.bin" > /dev/null
echo "$d/$name.bin"

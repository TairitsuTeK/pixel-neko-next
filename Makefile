CC = gcc
LD = ld
NASM = nasm
GRUB_MKRESCUE = grub-mkrescue
QEMU = qemu-system-i386

CFLAGS = -m32 -march=pentiumpro -ffreestanding -nostdlib -nostartfiles -fno-builtin \
         -fno-stack-protector -fno-pie -mno-mmx -mno-sse -mno-sse2 \
         -Os -Wall -Wextra -Iinclude -I.

LDFLAGS = -m elf_i386
ASMFLAGS = -f elf32

C_SOURCES = kernel/kernel.c \
            kernel/mm.c \
            kernel/idt.c \
            kernel/timer.c \
            kernel/cpu.c \
            kernel/splash.c \
            kernel/power.c \
            kernel/panic.c \
            kernel/image_dec.c \
            kernel/wallpaper.c \
            kernel/settings.c \
            kernel/audio.c \
            kernel/wav_dec.c \
            kernel/flac_dec.c \
            kernel/mp3_dec.c \
            drivers/ati_mach64.c \
            drivers/cmi8738.c \
            drivers/ct4810.c \
            drivers/gfx_accel.c \
            drivers/speaker.c \
            drivers/mouse.c \
            drivers/keyboard.c \
            drivers/disk.c \
            drivers/blkdev.c \
            drivers/isadma.c \
            drivers/fdc.c \
            drivers/font.c \
            wm/wm.c \
            apps/calc.c \
            apps/cpanel.c \
            apps/editor.c \
            apps/filemanager.c \
            apps/sysinfo.c \
            apps/puzzle.c \
            apps/imageviewer.c \
            apps/audioplayer.c \
            apps/installer.c \
            fs/fat.c \
            fs/part.c \
            fs/iso9660.c \
            fs/vfs.c

ASM_SOURCES = boot/multiboot.asm boot/vbe.asm boot/idt_asm.asm
HEADERS = $(wildcard include/*.h)

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

KERNEL_ELF = iso/boot/kernel.elf
VERSION_STR = $(shell grep "KERNEL_BUILD" include/version.h 2>/dev/null | head -1 | cut -d '"' -f 2)
ISO_FILE = pixel_neko_build_$(VERSION_STR).iso

.PHONY: all clean run debug rebuild

all: include/version.h $(ISO_FILE)

include/version.h: scripts/build_counter.sh
	@echo "  build counter..."
	@chmod +x scripts/build_counter.sh
	@./scripts/build_counter.sh

%.o: %.c $(HEADERS)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	@echo "  NASM  $<"
	$(NASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJECTS) link_grub.ld
	@echo "  LD    $@"
	@mkdir -p iso/boot/grub
	$(LD) $(LDFLAGS) -T link_grub.ld $(OBJECTS) -o $@

iso/boot/grub/grub.cfg:
	@mkdir -p iso/boot/grub
	@echo 'set timeout=20' > $@
	@echo 'set default=0' >> $@
	@echo '' >> $@
	@echo 'menuentry "Pixel Neko Next" {' >> $@
	@echo '    multiboot /boot/kernel.elf' >> $@
	@echo '    boot' >> $@
	@echo '}' >> $@

$(ISO_FILE): $(KERNEL_ELF) iso/boot/grub/grub.cfg
	@echo "  ISO   $@"
	@cp tools/grldr iso/boot/grldr
	@cp tools/grldr iso/grldr
	@cp tools/menu.lst iso/boot/menu.lst
	@cp tools/menu.lst iso/menu.lst
	$(GRUB_MKRESCUE) -o $@ iso/

run: $(ISO_FILE)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda disk.img -boot order=dc

debug: $(ISO_FILE)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda disk.img -s -S

clean:
	rm -f $(C_OBJECTS) $(ASM_OBJECTS)
	rm -f $(KERNEL_ELF)
	rm -f pixel_neko*.iso
	rm -rf iso

rebuild: clean all

# --- Toolchain ---
# Windows needs an ELF cross toolchain (e.g. i686-elf-tools). The native
# MinGW gcc/ld produce PE/COFF objects and cannot link this multiboot ELF
# kernel. Linux can use the native gcc/ld (which already emit ELF32).
#
# NOTE: On Windows we strongly recommend building inside MSYS2 or WSL2,
# because grub-mkrescue / nasm / shell utilities are not available in a
# plain cmd.exe environment.

ifeq ($(OS),Windows_NT)
TOOLCHAIN_PREFIX ?= i686-elf-
else
TOOLCHAIN_PREFIX ?=
endif

# Shell helpers. POSIX sh is assumed on every supported host (Linux, MSYS2,
# WSL2 or w64devkit); plain cmd.exe cannot run the build counter anyway.
RM    = rm -f
RMDIR = rm -rf
MKDIR = mkdir -p

CC            = $(TOOLCHAIN_PREFIX)gcc
LD            = $(TOOLCHAIN_PREFIX)ld
NASM          = nasm
GRUB_MKRESCUE = grub-mkrescue
QEMU          = qemu-system-i386

CFLAGS = -m32 -march=pentiumpro -ffreestanding -nostdlib -nostartfiles -fno-builtin \
         -fno-stack-protector -fno-pie -mno-mmx -mno-sse -mno-sse2 \
         -std=gnu11 -Os -Wall -Wextra -Iinclude -I.

LDFLAGS = -m elf_i386
ASMFLAGS = -f elf32 -Iinclude

C_SOURCES = kernel/kernel.c \
            kernel/mm.c \
            kernel/idt.c \
            kernel/irq.c \
            kernel/dma.c \
            kernel/driver.c \
            kernel/log.c \
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
            drivers/pci.c \
            drivers/vmware_svga.c \
            drivers/hdaudio.c \
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
            drivers/usb_core.c \
            drivers/usb_ehci.c \
            drivers/usb_uhci.c \
            drivers/usb_hid.c \
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

# Recursive header discovery, but exclude the generated version header so
# that bumping the build counter does not force a full rebuild.
HEADERS = $(filter-out include/version.h,$(wildcard include/*.h include/*/*.h))

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

KERNEL_ELF = iso/boot/kernel.elf

# Generated version header lives under build/generated and is only used
# through -Ibuild/generated. include/version.h is NOT regenerated.
GEN_DIR      = build/generated
GEN_VERSION  = $(GEN_DIR)/version.h

# Fixed ISO name so the first and subsequent builds produce the same file.
# The build counter / version string is still written into the kernel and
# grub menu, but no longer affects the output filename.
ISO_FILE = pixel_neko.iso

# Disk image used by `make run` / `make debug`.
DISK_IMG = disk.img
DISK_SIZE = 64M

.PHONY: all clean run run-kernel run-usb debug rebuild $(GEN_VERSION)

all: $(GEN_VERSION) $(ISO_FILE)

# Always run the build counter. It writes to build/generated/version.h.
# include/version.h stays untouched so the checked-in version is stable.
$(GEN_VERSION): scripts/build_counter.sh
	@echo "  build counter..."
	@$(MKDIR) $(GEN_DIR)
	@VERSION_OUT=$(GEN_VERSION) sh scripts/build_counter.sh

# Make sure every object depends on the generated header but NOT in a way
# that causes a full rebuild when only the counter value changes. The
# generated header is order-only for most sources; only files that
# actually include it will rebuild, and since we keep the content stable
# when nothing changes, this is cheap.
%.o: %.c $(HEADERS) | $(GEN_VERSION)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -I$(GEN_DIR) -c $< -o $@

%.o: %.asm
	@echo "  NASM  $<"
	$(NASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJECTS) link_grub.ld
	@echo "  LD    $@"
	@$(MKDIR) iso/boot/grub
	$(LD) $(LDFLAGS) -T link_grub.ld $(OBJECTS) -o $@

iso/boot/grub/grub.cfg: Makefile
	@$(MKDIR) iso/boot/grub
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

$(DISK_IMG):
	@echo "  IMG   $@ ($(DISK_SIZE))"
	qemu-img create -f raw $(DISK_IMG) $(DISK_SIZE)

run: $(ISO_FILE) $(DISK_IMG)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda $(DISK_IMG) -boot order=dc

# Boot the ELF directly with QEMU (no grub-mkrescue / ISO required).
run-kernel: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -m 128M

# Test USB: attach a UHCI controller + USB keyboard/mouse and capture the
# kernel's serial (COM1) log, where the USB enumeration messages are printed.
run-usb: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -m 128M \
		-device piix3-usb-uhci -device usb-kbd -device usb-mouse \
		-serial file:usb_log.txt -monitor none

debug: $(ISO_FILE) $(DISK_IMG)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda $(DISK_IMG) -s -S

clean:
	$(RM) $(C_OBJECTS) $(ASM_OBJECTS)
	$(RM) $(KERNEL_ELF)
	$(RM) pixel_neko*.iso
	$(RMDIR) build
	$(RMDIR) iso

rebuild: clean all

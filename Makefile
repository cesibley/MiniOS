ARCH      := x86_64
EFIINC    := /usr/include/efi
EFIINCARCH:= /usr/include/efi/$(ARCH)

EFILDS    := $(shell find /usr -name 'elf_$(ARCH)_efi.lds' 2>/dev/null | head -n 1)
CRT0      := $(shell find /usr -name 'crt0-efi-$(ARCH).o' 2>/dev/null | head -n 1)
LIBGNUEFI := $(shell find /usr -name 'libgnuefi.a' 2>/dev/null | head -n 1)
LIBEFI    := $(shell find /usr -name 'libefi.a' 2>/dev/null | head -n 1)

CC        := gcc
LD        := ld
OBJCOPY   := objcopy

CFLAGS    := -I$(EFIINC) -I$(EFIINCARCH) \
             -fpic -fshort-wchar -mno-red-zone \
             -ffreestanding -fno-stack-protector -fno-stack-check \
             -Wall -Wextra
PLAYVID_CFLAGS := $(CFLAGS) -O2

LDFLAGS   := -nostdlib -znocombreloc -T $(EFILDS) \
             -shared -Bsymbolic

TARGET    := BOOTX64.EFI
INTERMED  := boot.so
OBJECTS   := boot.o
PI_TARGET := PIX64.EFI
PI_INTERMED := pi.so
PI_OBJECTS := pi.o
GFX_TARGET := GFXTEST.EFI
GFX_INTERMED := gfxtest.so
GFX_OBJECTS := gfxtest.o
CLOCK_TARGET := CLOCKX64.EFI
CLOCK_INTERMED := clock.so
CLOCK_OBJECTS := clock.o
HEX_TARGET := HEXVIEW.EFI
HEX_INTERMED := hexview.so
HEX_OBJECTS := hexview.o
EDIT_TARGET := EDIT.EFI
EDIT_INTERMED := textedit.so
EDIT_OBJECTS := textedit.o
GFXCLOCK_TARGET := GFXCLOCK.EFI
GFXCLOCK_INTERMED := gfxclock.so
GFXCLOCK_OBJECTS := gfxclock.o
SUNMAP_TARGET := SUNMAP.EFI
SUNMAP_INTERMED := sunmap.so
SUNMAP_OBJECTS := sunmap.o
GOPQUERY_TARGET := GOPQUERY.EFI
GOPQUERY_INTERMED := gopquery.so
GOPQUERY_OBJECTS := gopquery.o
IMGVIEW_TARGET := IMGVIEW.EFI
IMGVIEW_INTERMED := imgview.so
IMGVIEW_OBJECTS := imgview.o
PLAYVID_TARGET := PLAYVID.EFI
PLAYVID_INTERMED := playvid.so
PLAYVID_OBJECTS := playvid.o

all: check $(TARGET) $(PI_TARGET) $(GFX_TARGET) $(CLOCK_TARGET) $(HEX_TARGET) $(EDIT_TARGET) $(GFXCLOCK_TARGET) $(SUNMAP_TARGET) $(GOPQUERY_TARGET) $(IMGVIEW_TARGET) $(PLAYVID_TARGET)

check:
	@test -n "$(EFILDS)" || (echo "Missing elf_$(ARCH)_efi.lds. Install gnu-efi."; exit 1)
	@test -n "$(CRT0)" || (echo "Missing crt0-efi-$(ARCH).o. Install gnu-efi."; exit 1)
	@test -n "$(LIBGNUEFI)" || (echo "Missing libgnuefi.a. Install gnu-efi."; exit 1)
	@test -n "$(LIBEFI)" || (echo "Missing libefi.a. Install gnu-efi."; exit 1)
	@test -d "$(EFIINC)" || (echo "Missing $(EFIINC). Install gnu-efi development headers."; exit 1)

boot.o: boot.c
	$(CC) $(CFLAGS) -c $< -o $@

$(INTERMED): $(OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(TARGET): $(INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	mkdir -p iso_root/EFI/BOOT
	cp -f $@ iso_root/$@
	cp -f $@ iso_root/EFI/BOOT/$@

clean:
	rm -f $(OBJECTS) $(INTERMED) $(TARGET) \
	      $(PI_OBJECTS) $(PI_INTERMED) $(PI_TARGET) \
	      $(GFX_OBJECTS) $(GFX_INTERMED) $(GFX_TARGET) \
	      $(CLOCK_OBJECTS) $(CLOCK_INTERMED) $(CLOCK_TARGET) \
	      $(HEX_OBJECTS) $(HEX_INTERMED) $(HEX_TARGET) \
	      $(EDIT_OBJECTS) $(EDIT_INTERMED) $(EDIT_TARGET) \
	      $(GFXCLOCK_OBJECTS) $(GFXCLOCK_INTERMED) $(GFXCLOCK_TARGET) \
	      $(SUNMAP_OBJECTS) $(SUNMAP_INTERMED) $(SUNMAP_TARGET) \
		      $(GOPQUERY_OBJECTS) $(GOPQUERY_INTERMED) $(GOPQUERY_TARGET) \
		      $(IMGVIEW_OBJECTS) $(IMGVIEW_INTERMED) $(IMGVIEW_TARGET) \
		      $(PLAYVID_OBJECTS) $(PLAYVID_INTERMED) $(PLAYVID_TARGET) \
		      iso_root/$(TARGET) iso_root/$(PI_TARGET) iso_root/$(GFX_TARGET) \
	      iso_root/$(CLOCK_TARGET) iso_root/$(HEX_TARGET) iso_root/$(EDIT_TARGET) \
	      iso_root/$(GFXCLOCK_TARGET) iso_root/$(SUNMAP_TARGET) \
	      iso_root/$(GOPQUERY_TARGET) iso_root/$(IMGVIEW_TARGET) \
	      iso_root/$(PLAYVID_TARGET) \
	      iso_root/EFI/BOOT/$(TARGET)

run-info:
	@echo "Copy $(TARGET) to:"
	@echo "  EFI/BOOT/BOOTX64.EFI"
	@echo "Optional PI tool:"
	@echo "  EFI/BOOT/PIX64.EFI"
	@echo "Optional graphics tool:"
	@echo "  EFI/BOOT/GFXTEST.EFI"
	@echo "Optional clock tool:"
	@echo "  EFI/BOOT/CLOCKX64.EFI"
	@echo "Optional hex viewer:"
	@echo "  EFI/BOOT/HEXVIEW.EFI"
	@echo "Optional text editor:"
	@echo "  EFI/BOOT/EDIT.EFI"
	@echo "Optional full-screen graphics clock:"
	@echo "  EFI/BOOT/GFXCLOCK.EFI"
	@echo "Optional world illumination map demo:"
	@echo "  EFI/BOOT/SUNMAP.EFI"
	@echo "Optional GOP query tool:"
	@echo "  EFI/BOOT/GOPQUERY.EFI"
	@echo "Optional image viewer:"
	@echo "  EFI/BOOT/IMGVIEW.EFI"
	@echo "Optional MPG video player:"
	@echo "  EFI/BOOT/PLAYVID.EFI"

pi.o: pi.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PI_INTERMED): $(PI_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(PI_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(PI_TARGET): $(PI_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

gfxtest.o: gfxtest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(GFX_INTERMED): $(GFX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFX_TARGET): $(GFX_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

clock.o: clock.c
	$(CC) $(CFLAGS) -c $< -o $@

$(CLOCK_INTERMED): $(CLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(CLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(CLOCK_TARGET): $(CLOCK_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

hexview.o: hexview.c
	$(CC) $(CFLAGS) -c $< -o $@

$(HEX_INTERMED): $(HEX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(HEX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(HEX_TARGET): $(HEX_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

textedit.o: textedit.c
	$(CC) $(CFLAGS) -c $< -o $@

$(EDIT_INTERMED): $(EDIT_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(EDIT_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(EDIT_TARGET): $(EDIT_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

gfxclock.o: gfxclock.c
	$(CC) $(CFLAGS) -c $< -o $@

$(GFXCLOCK_INTERMED): $(GFXCLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFXCLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFXCLOCK_TARGET): $(GFXCLOCK_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

sunmap.o: sunmap.c
	$(CC) $(CFLAGS) -c $< -o $@

$(SUNMAP_INTERMED): $(SUNMAP_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(SUNMAP_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(SUNMAP_TARGET): $(SUNMAP_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

gopquery.o: gopquery.c
	$(CC) $(CFLAGS) -c $< -o $@

$(GOPQUERY_INTERMED): $(GOPQUERY_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GOPQUERY_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GOPQUERY_TARGET): $(GOPQUERY_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

.PHONY: all clean check run-info

imgview.o: imgview.c
	$(CC) $(CFLAGS) -c $< -o $@

$(IMGVIEW_INTERMED): $(IMGVIEW_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(IMGVIEW_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(IMGVIEW_TARGET): $(IMGVIEW_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

playvid.o: playvid.c
	$(CC) $(PLAYVID_CFLAGS) -c $< -o $@

$(PLAYVID_INTERMED): $(PLAYVID_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(PLAYVID_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(PLAYVID_TARGET): $(PLAYVID_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

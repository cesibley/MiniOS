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

LDFLAGS   := -nostdlib -znocombreloc -T $(EFILDS) \
             -shared -Bsymbolic

TARGET    := BOOTX64.EFI
BUILD_DIR := build
INTERMED  := $(BUILD_DIR)/boot.so
OBJECTS   := $(BUILD_DIR)/boot.o
PI_TARGET := PI.EFI
PI_INTERMED := $(BUILD_DIR)/pi.so
PI_OBJECTS := $(BUILD_DIR)/pi.o
GFX_TARGET := GFXTEST.EFI
GFX_INTERMED := $(BUILD_DIR)/gfxtest.so
GFX_OBJECTS := $(BUILD_DIR)/gfxtest.o
CLOCK_TARGET := CLOCKX64.EFI
CLOCK_INTERMED := $(BUILD_DIR)/clock.so
CLOCK_OBJECTS := $(BUILD_DIR)/clock.o
EDIT_TARGET := EDITTEXT.EFI
EDIT_INTERMED := $(BUILD_DIR)/textedit.so
EDIT_OBJECTS := $(BUILD_DIR)/textedit.o
GFXCLOCK_TARGET := GFXCLOCK.EFI
GFXCLOCK_INTERMED := $(BUILD_DIR)/gfxclock.so
GFXCLOCK_OBJECTS := $(BUILD_DIR)/gfxclock.o
SUNMAP_TARGET := SUNMAP.EFI
SUNMAP_INTERMED := $(BUILD_DIR)/sunmap.so
SUNMAP_OBJECTS := $(BUILD_DIR)/sunmap.o
GOPQUERY_TARGET := GOPQUERY.EFI
GOPQUERY_INTERMED := $(BUILD_DIR)/gopquery.so
GOPQUERY_OBJECTS := $(BUILD_DIR)/gopquery.o
VIEW_TARGET := VIEW.EFI
VIEW_INTERMED := $(BUILD_DIR)/view.so
VIEW_OBJECTS := $(BUILD_DIR)/view.o
META_TARGET := META.EFI
META_INTERMED := $(BUILD_DIR)/meta.so
META_OBJECTS := $(BUILD_DIR)/meta.o

all: check $(TARGET) $(PI_TARGET) $(GFX_TARGET) $(CLOCK_TARGET) $(GFXCLOCK_TARGET) $(SUNMAP_TARGET) $(GOPQUERY_TARGET) $(VIEW_TARGET) $(META_TARGET)

check:
	@test -n "$(EFILDS)" || (echo "Missing elf_$(ARCH)_efi.lds. Install gnu-efi."; exit 1)
	@test -n "$(CRT0)" || (echo "Missing crt0-efi-$(ARCH).o. Install gnu-efi."; exit 1)
	@test -n "$(LIBGNUEFI)" || (echo "Missing libgnuefi.a. Install gnu-efi."; exit 1)
	@test -n "$(LIBEFI)" || (echo "Missing libefi.a. Install gnu-efi."; exit 1)
	@test -d "$(EFIINC)" || (echo "Missing $(EFIINC). Install gnu-efi development headers."; exit 1)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/boot.o: boot.c | $(BUILD_DIR)
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
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET) \
	      $(PI_OBJECTS) $(PI_INTERMED) $(PI_TARGET) \
	      $(GFX_OBJECTS) $(GFX_INTERMED) $(GFX_TARGET) \
	      $(CLOCK_OBJECTS) $(CLOCK_INTERMED) $(CLOCK_TARGET) \
	      $(EDIT_OBJECTS) $(EDIT_INTERMED) $(EDIT_TARGET) \
	      $(GFXCLOCK_OBJECTS) $(GFXCLOCK_INTERMED) $(GFXCLOCK_TARGET) \
	      $(SUNMAP_OBJECTS) $(SUNMAP_INTERMED) $(SUNMAP_TARGET) \
	      $(GOPQUERY_OBJECTS) $(GOPQUERY_INTERMED) $(GOPQUERY_TARGET) \
	      $(VIEW_OBJECTS) $(VIEW_INTERMED) $(VIEW_TARGET) \
	      $(META_OBJECTS) $(META_INTERMED) $(META_TARGET) \
	      iso_root/$(TARGET) iso_root/$(PI_TARGET) iso_root/$(GFX_TARGET) \
	      iso_root/$(CLOCK_TARGET) iso_root/$(EDIT_TARGET) \
	      iso_root/$(GFXCLOCK_TARGET) iso_root/$(SUNMAP_TARGET) \
	      iso_root/$(GOPQUERY_TARGET) iso_root/$(VIEW_TARGET) \
	      iso_root/$(META_TARGET) \
	      iso_root/EFI/BOOT/$(TARGET)

run-info:
	@echo "Copy $(TARGET) to:"
	@echo "  EFI/BOOT/BOOTX64.EFI"
	@echo "Optional PI tool:"
	@echo "  EFI/BOOT/PI.EFI"
	@echo "Optional graphics tool:"
	@echo "  EFI/BOOT/GFXTEST.EFI"
	@echo "Optional clock tool:"
	@echo "  EFI/BOOT/CLOCKX64.EFI"
	@echo "Optional text editor:"
	@echo "  EFI/BOOT/EDITTEXT.EFI"
	@echo "Optional full-screen graphics clock:"
	@echo "  EFI/BOOT/GFXCLOCK.EFI"
	@echo "Optional world illumination map demo:"
	@echo "  EFI/BOOT/SUNMAP.EFI"
	@echo "Optional GOP query tool:"
	@echo "  EFI/BOOT/GOPQUERY.EFI"
	@echo "Optional universal viewer:"
	@echo "  EFI/BOOT/VIEW.EFI"
	@echo "Optional metadata editor:"
	@echo "  EFI/BOOT/META.EFI"

$(BUILD_DIR)/pi.o: pi.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(PI_INTERMED): $(PI_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(PI_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(PI_TARGET): $(PI_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

$(BUILD_DIR)/gfxtest.o: gfxtest.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(GFX_INTERMED): $(GFX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFX_TARGET): $(GFX_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

$(BUILD_DIR)/clock.o: clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLOCK_INTERMED): $(CLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(CLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(CLOCK_TARGET): $(CLOCK_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@


$(BUILD_DIR)/textedit.o: textedit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EDIT_INTERMED): $(EDIT_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(EDIT_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(EDIT_TARGET): $(EDIT_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

$(BUILD_DIR)/gfxclock.o: gfxclock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(GFXCLOCK_INTERMED): $(GFXCLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFXCLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFXCLOCK_TARGET): $(GFXCLOCK_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

$(BUILD_DIR)/sunmap.o: sunmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SUNMAP_INTERMED): $(SUNMAP_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(SUNMAP_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(SUNMAP_TARGET): $(SUNMAP_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

$(BUILD_DIR)/gopquery.o: gopquery.c | $(BUILD_DIR)
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


$(BUILD_DIR)/view.o: view.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(VIEW_INTERMED): $(VIEW_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(VIEW_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(VIEW_TARGET): $(VIEW_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@


$(BUILD_DIR)/meta.o: meta.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(META_INTERMED): $(META_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(META_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(META_TARGET): $(META_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	cp -f $@ iso_root/$@

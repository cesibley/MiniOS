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

TARGET    := bootx64.efi
ISO_ROOT  := iso_root
BOOT_PATH := $(ISO_ROOT)/$(TARGET)
BUILD_DIR := build
INTERMED  := $(BUILD_DIR)/boot.so
OBJECTS   := $(BUILD_DIR)/boot.o
PI_TARGET := pi
PI_PATH := $(ISO_ROOT)/$(PI_TARGET)
PI_INTERMED := $(BUILD_DIR)/pi.so
PI_OBJECTS := $(BUILD_DIR)/pi.o
GFX_TARGET := gfxtest
GFX_PATH := $(ISO_ROOT)/$(GFX_TARGET)
GFX_INTERMED := $(BUILD_DIR)/gfxtest.so
GFX_OBJECTS := $(BUILD_DIR)/gfxtest.o
CLOCK_TARGET := clockx64
CLOCK_PATH := $(ISO_ROOT)/$(CLOCK_TARGET)
CLOCK_INTERMED := $(BUILD_DIR)/clock.so
CLOCK_OBJECTS := $(BUILD_DIR)/clock.o
EDIT_TARGET := edit
EDIT_PATH := $(ISO_ROOT)/$(EDIT_TARGET)
EDIT_INTERMED := $(BUILD_DIR)/edit.so
EDIT_OBJECTS := $(BUILD_DIR)/edit.o
GFXCLOCK_TARGET := gfxclock
GFXCLOCK_PATH := $(ISO_ROOT)/$(GFXCLOCK_TARGET)
GFXCLOCK_INTERMED := $(BUILD_DIR)/gfxclock.so
GFXCLOCK_OBJECTS := $(BUILD_DIR)/gfxclock.o
SUNMAP_TARGET := sunmap
SUNMAP_PATH := $(ISO_ROOT)/$(SUNMAP_TARGET)
SUNMAP_INTERMED := $(BUILD_DIR)/sunmap.so
SUNMAP_OBJECTS := $(BUILD_DIR)/sunmap.o
GOPQUERY_TARGET := gopquery
GOPQUERY_PATH := $(ISO_ROOT)/$(GOPQUERY_TARGET)
GOPQUERY_INTERMED := $(BUILD_DIR)/gopquery.so
GOPQUERY_OBJECTS := $(BUILD_DIR)/gopquery.o
VIEW_TARGET := view
VIEW_PATH := $(ISO_ROOT)/$(VIEW_TARGET)
VIEW_INTERMED := $(BUILD_DIR)/view.so
VIEW_OBJECTS := $(BUILD_DIR)/view.o
META_TARGET := meta
META_PATH := $(ISO_ROOT)/$(META_TARGET)
META_INTERMED := $(BUILD_DIR)/meta.so
META_OBJECTS := $(BUILD_DIR)/meta.o
MOUSE_TARGET := mouse
MOUSE_PATH := $(ISO_ROOT)/$(MOUSE_TARGET)
MOUSE_INTERMED := $(BUILD_DIR)/mouse.so
MOUSE_OBJECTS := $(BUILD_DIR)/mouse.o
LAUNCHER_TARGET := launcher
LAUNCHER_PATH := $(ISO_ROOT)/$(LAUNCHER_TARGET)
LAUNCHER_INTERMED := $(BUILD_DIR)/launcher.so
LAUNCHER_OBJECTS := $(BUILD_DIR)/launcher.o
TTF_TARGET := ttfdemo
TTF_PATH := $(ISO_ROOT)/$(TTF_TARGET)
TTF_INTERMED := $(BUILD_DIR)/ttfdemo.so
TTF_OBJECTS := $(BUILD_DIR)/ttfdemo.o
AUX_EFI_PATHS := $(PI_PATH) $(GFX_PATH) $(CLOCK_PATH) $(EDIT_PATH) $(GFXCLOCK_PATH) $(SUNMAP_PATH) $(GOPQUERY_PATH) $(VIEW_PATH) $(META_PATH) $(MOUSE_PATH)

define install_program_meta
	mkdir -p $(ISO_ROOT)/.meta
	@test -f $(ISO_ROOT)/.meta/$(notdir $(basename $1)).meta || \
		printf "TYPE: Program\n" > $(ISO_ROOT)/.meta/$(notdir $(basename $1)).meta
endef

all: check $(BOOT_PATH) $(PI_PATH) $(GFX_PATH) $(CLOCK_PATH) $(EDIT_PATH) $(GFXCLOCK_PATH) $(SUNMAP_PATH) $(GOPQUERY_PATH) $(VIEW_PATH) $(META_PATH) $(MOUSE_PATH) $(LAUNCHER_PATH) $(TTF_PATH)

$(TARGET): $(BOOT_PATH)
$(PI_TARGET): $(PI_PATH)
$(GFX_TARGET): $(GFX_PATH)
$(CLOCK_TARGET): $(CLOCK_PATH)
$(EDIT_TARGET): $(EDIT_PATH)
$(GFXCLOCK_TARGET): $(GFXCLOCK_PATH)
$(SUNMAP_TARGET): $(SUNMAP_PATH)
$(GOPQUERY_TARGET): $(GOPQUERY_PATH)
$(VIEW_TARGET): $(VIEW_PATH)
$(META_TARGET): $(META_PATH)
$(MOUSE_TARGET): $(MOUSE_PATH)
$(LAUNCHER_TARGET): $(LAUNCHER_PATH)
$(TTF_TARGET): $(TTF_PATH)

check:
	@test -n "$(EFILDS)" || (echo "Missing elf_$(ARCH)_efi.lds. Install gnu-efi."; exit 1)
	@test -n "$(CRT0)" || (echo "Missing crt0-efi-$(ARCH).o. Install gnu-efi."; exit 1)
	@test -n "$(LIBGNUEFI)" || (echo "Missing libgnuefi.a. Install gnu-efi."; exit 1)
	@test -n "$(LIBEFI)" || (echo "Missing libefi.a. Install gnu-efi."; exit 1)
	@test -d "$(EFIINC)" || (echo "Missing $(EFIINC). Install gnu-efi development headers."; exit 1)

$(BUILD_DIR):
	mkdir -p $@

$(ISO_ROOT):
	mkdir -p $@

$(BUILD_DIR)/boot.o: boot.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(INTERMED): $(OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(BOOT_PATH): $(INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp -f $@ $(ISO_ROOT)/EFI/BOOT/$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(PI_OBJECTS) $(PI_INTERMED) \
	      $(GFX_OBJECTS) $(GFX_INTERMED) \
	      $(CLOCK_OBJECTS) $(CLOCK_INTERMED) \
	      $(EDIT_OBJECTS) $(EDIT_INTERMED) \
	      $(GFXCLOCK_OBJECTS) $(GFXCLOCK_INTERMED) \
	      $(SUNMAP_OBJECTS) $(SUNMAP_INTERMED) \
	      $(GOPQUERY_OBJECTS) $(GOPQUERY_INTERMED) \
	      $(VIEW_OBJECTS) $(VIEW_INTERMED) \
	      $(META_OBJECTS) $(META_INTERMED) \
	      $(MOUSE_OBJECTS) $(MOUSE_INTERMED) \
	      $(TTF_OBJECTS) $(TTF_INTERMED) \
	      $(BOOT_PATH) $(PI_PATH) $(GFX_PATH) \
	      $(CLOCK_PATH) $(EDIT_PATH) \
	      $(GFXCLOCK_PATH) $(SUNMAP_PATH) \
	      $(GOPQUERY_PATH) $(VIEW_PATH) \
	      $(META_PATH) $(MOUSE_PATH) $(LAUNCHER_PATH) $(TTF_PATH) \
	      $(ISO_ROOT)/EFI/BOOT/$(TARGET)

run-info:
	@echo "Copy $(TARGET) to:"
	@echo "  EFI/BOOT/bootx64.efi"
	@echo "Optional PI tool:"
	@echo "  EFI/BOOT/pi"
	@echo "Optional graphics tool:"
	@echo "  EFI/BOOT/gfxtest"
	@echo "Optional clock tool:"
	@echo "  EFI/BOOT/clockx64"
	@echo "Optional text editor:"
	@echo "  EFI/BOOT/edit"
	@echo "Optional full-screen graphics clock:"
	@echo "  EFI/BOOT/gfxclock"
	@echo "Optional world illumination map demo:"
	@echo "  EFI/BOOT/sunmap"
	@echo "Optional GOP query tool:"
	@echo "  EFI/BOOT/gopquery"
	@echo "Optional universal viewer:"
	@echo "  EFI/BOOT/view"
	@echo "Optional metadata editor:"
	@echo "  EFI/BOOT/meta"
	@echo "Optional mouse test tool:"
	@echo "  EFI/BOOT/mouse"

$(BUILD_DIR)/pi.o: pi.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(PI_INTERMED): $(PI_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(PI_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(PI_PATH): $(PI_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

$(BUILD_DIR)/gfxtest.o: gfxtest.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(GFX_INTERMED): $(GFX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFX_PATH): $(GFX_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

$(BUILD_DIR)/clock.o: clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLOCK_INTERMED): $(CLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(CLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(CLOCK_PATH): $(CLOCK_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)


$(BUILD_DIR)/edit.o: edit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EDIT_INTERMED): $(EDIT_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(EDIT_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(EDIT_PATH): $(EDIT_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

$(BUILD_DIR)/gfxclock.o: gfxclock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(GFXCLOCK_INTERMED): $(GFXCLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFXCLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFXCLOCK_PATH): $(GFXCLOCK_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

$(BUILD_DIR)/sunmap.o: sunmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SUNMAP_INTERMED): $(SUNMAP_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(SUNMAP_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(SUNMAP_PATH): $(SUNMAP_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

$(BUILD_DIR)/gopquery.o: gopquery.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(GOPQUERY_INTERMED): $(GOPQUERY_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GOPQUERY_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GOPQUERY_PATH): $(GOPQUERY_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

.PHONY: all clean check run-info \
	$(TARGET) $(PI_TARGET) $(GFX_TARGET) $(CLOCK_TARGET) $(EDIT_TARGET) \
	$(GFXCLOCK_TARGET) $(SUNMAP_TARGET) $(GOPQUERY_TARGET) $(VIEW_TARGET) $(META_TARGET) $(MOUSE_TARGET) $(LAUNCHER_TARGET) $(TTF_TARGET)

$(BUILD_DIR)/launcher.o: launcher.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LAUNCHER_INTERMED): $(LAUNCHER_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(LAUNCHER_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(LAUNCHER_PATH): $(LAUNCHER_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)


$(BUILD_DIR)/view.o: view.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(VIEW_INTERMED): $(VIEW_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(VIEW_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(VIEW_PATH): $(VIEW_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)


$(BUILD_DIR)/meta.o: meta.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(META_INTERMED): $(META_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(META_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(META_PATH): $(META_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)


$(BUILD_DIR)/mouse.o: mouse.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(MOUSE_INTERMED): $(MOUSE_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(MOUSE_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(MOUSE_PATH): $(MOUSE_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)


$(BUILD_DIR)/ttfdemo.o: stb_truetype_sample.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TTF_INTERMED): $(TTF_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(TTF_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(TTF_PATH): $(TTF_INTERMED) | $(ISO_ROOT)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@
	$(call install_program_meta,$@)

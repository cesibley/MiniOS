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

all: check $(TARGET) $(PI_TARGET) $(GFX_TARGET) $(CLOCK_TARGET) $(HEX_TARGET)

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

clean:
	rm -f $(OBJECTS) $(INTERMED) $(TARGET) \
	      $(PI_OBJECTS) $(PI_INTERMED) $(PI_TARGET) \
	      $(GFX_OBJECTS) $(GFX_INTERMED) $(GFX_TARGET) \
	      $(CLOCK_OBJECTS) $(CLOCK_INTERMED) $(CLOCK_TARGET) \
	      $(HEX_OBJECTS) $(HEX_INTERMED) $(HEX_TARGET)

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

pi.o: pi.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PI_INTERMED): $(PI_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(PI_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(PI_TARGET): $(PI_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@

gfxtest.o: gfxtest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(GFX_INTERMED): $(GFX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(GFX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(GFX_TARGET): $(GFX_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@

clock.o: clock.c
	$(CC) $(CFLAGS) -c $< -o $@

$(CLOCK_INTERMED): $(CLOCK_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(CLOCK_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(CLOCK_TARGET): $(CLOCK_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@

hexview.o: hexview.c
	$(CC) $(CFLAGS) -c $< -o $@

$(HEX_INTERMED): $(HEX_OBJECTS)
	$(LD) $(LDFLAGS) $(CRT0) $(HEX_OBJECTS) -o $@ $(LIBGNUEFI) $(LIBEFI)

$(HEX_TARGET): $(HEX_INTERMED)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-$(ARCH) $< $@

.PHONY: all clean check run-info

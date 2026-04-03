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

all: check $(TARGET)

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
	rm -f $(OBJECTS) $(INTERMED) $(TARGET)

run-info:
	@echo "Copy $(TARGET) to:"
	@echo "  EFI/BOOT/BOOTX64.EFI"

.PHONY: all clean check run-info

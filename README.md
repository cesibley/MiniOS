# MiniOS (UEFI)

This project builds a tiny **x86_64 UEFI application** (`BOOTX64.EFI`) that prints:

`MiniOS UEFI booted successfully!`

and then halts in an infinite loop.

## Build

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential gnu-efi mtools
make
```

The build outputs `BOOTX64.EFI` in the repo root.
It also builds `PIX64.EFI`, a standalone UEFI app that computes π digits with a spigot algorithm.
It also builds `GFXTEST.EFI`, a standalone UEFI app that queries GOP modes and draws color bars.
It also builds `CLOCKX64.EFI`, a standalone UEFI app that prints the current UEFI clock time.
It also builds `HEXVIEW.EFI`, a standalone UEFI app that prints a hex/ASCII view of a file.

## Build only the PI app

If you only want the PI calculator:

```bash
make check
make PIX64.EFI
```

### What the PI app does

- Prompts for decimal places (1-5000)
- Computes π using an integer spigot algorithm
- Prints `3.` followed by the requested digits

## Boot in VirtualBox (UEFI mode)

### 1) Create a FAT EFI disk image containing `EFI/BOOT/BOOTX64.EFI`

From the repo root:

```bash
# Build first
make

# Create a small FAT image (64 MiB)
truncate -s 64M esp.img
mkfs.vfat esp.img

# Create UEFI fallback path inside image and copy bootloader
mmd -i esp.img ::/EFI ::/EFI/BOOT
mcopy -i esp.img BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img PIX64.EFI ::/EFI/BOOT/PIX64.EFI
mcopy -i esp.img GFXTEST.EFI ::/EFI/BOOT/GFXTEST.EFI
mcopy -i esp.img CLOCKX64.EFI ::/EFI/BOOT/CLOCKX64.EFI
mcopy -i esp.img HEXVIEW.EFI ::/EFI/BOOT/HEXVIEW.EFI
```

### 2) Create and configure a VM in VirtualBox

- **Type**: Other
- **Version**: Other/Unknown (64-bit)
- **RAM**: 256 MB+ is enough
- **Disk**: use an existing disk and select `esp.img`

Then run these commands to force firmware + storage defaults that work well for UEFI testing:

```bash
VBoxManage modifyvm "MiniOS" --firmware efi
VBoxManage storagectl "MiniOS" --name "SATA" --add sata --controller IntelAhci
VBoxManage storageattach "MiniOS" --storagectl "SATA" --port 0 --device 0 \
  --type hdd --medium "$(pwd)/esp.img"
```

### 3) Boot

Start the VM. UEFI should automatically load `EFI/BOOT/BOOTX64.EFI` and print the message.

From the MiniOS shell, launch the PI program with:

```text
run PIX64.EFI
```

To launch the graphics demo:

```text
run GFXTEST.EFI
```

To launch the clock demo:

```text
run CLOCKX64.EFI
```

To launch the hex viewer:

```text
run HEXVIEW.EFI
```

## Troubleshooting

- If build fails with missing `elf_x86_64_efi.lds`, `crt0-efi-x86_64.o`, or `libefi`/`libgnuefi`, install the `gnu-efi` development package.
- If the VM does not boot your file, verify the exact fallback path is `EFI/BOOT/BOOTX64.EFI`.
- If Secure Boot is enabled in your VM firmware, disable it for this unsigned test binary.

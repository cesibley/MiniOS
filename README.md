# MiniOS (UEFI)

MiniOS is a tiny **x86_64 UEFI playground** built with `gnu-efi`.

The default app (`BOOTX64.EFI`) starts an interactive MiniOS UEFI shell.

On startup it prints:

`MiniOS UEFI shell`

and then shows a `MiniOS>` prompt. Use `help` to list available shell commands
(e.g. `list`, `read`, `write`, `memmap`, `meminfo`, `run`, `reboot`, `halt`).

### Built-in shell commands

- `help` — show the built-in command list.
- `cls` — clear the screen and reset cursor position.
- `echo TEXT` — print `TEXT` exactly as entered.
- `list [PATH]` — list entries for a directory, or metadata for a file path.
- `read FILE` — display the contents of `FILE`.
- `write FILE TEXT` — overwrite `FILE` with `TEXT`.
- `memmap` — print the full UEFI memory map descriptors.
- `meminfo` — print memory totals grouped by UEFI memory type.
- `run EFI_FILE` — load and execute another EFI application.
- `reboot` — reboot the machine via UEFI `ResetSystem`.
- `halt` — print a halt message and stop execution in an infinite loop.

The current build also produces standalone UEFI utilities:

- `PIX64.EFI` — computes π digits with a spigot algorithm
- `GFXTEST.EFI` — queries GOP modes and draws color bars
- `CLOCKX64.EFI` — prints the current UEFI clock time
- `HEXVIEW.EFI` — prints a hex/ASCII view of a file

## Latest project configuration

The repo is currently configured to:

- build all EFI binaries with `make`
- stage runnable EFI files in `iso_root/`
- boot with QEMU + OVMF using `./StartMiniOS`

`StartMiniOS` currently runs:

```bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive file=fat:rw:iso_root,format=raw
```

So QEMU mounts `iso_root` as a writable FAT drive and OVMF loads the UEFI programs from there.

## Requirements (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential gnu-efi qemu-system-x86 ovmf mtools
```

## Build

From the repo root:

```bash
make
```

This runs `check` first (verifies `gnu-efi` linker script/libraries/headers) and then builds:

- `BOOTX64.EFI`
- `PIX64.EFI`
- `GFXTEST.EFI`
- `CLOCKX64.EFI`
- `HEXVIEW.EFI`

## Run with the current QEMU configuration

```bash
./StartMiniOS
```

Inside the UEFI shell, run apps such as:

```text
run BOOTX64.EFI
run PIX64.EFI
run GFXTEST.EFI
run CLOCKX64.EFI
run HEXVIEW.EFI
```

## Build only one app

Example for PI:

```bash
make check
make PIX64.EFI
```

## Optional: VirtualBox flow

If you prefer VirtualBox, create a FAT image and copy `BOOTX64.EFI` to `EFI/BOOT/BOOTX64.EFI` as fallback.

## Troubleshooting

- Missing `elf_x86_64_efi.lds`, `crt0-efi-x86_64.o`, `libefi.a`, or `libgnuefi.a` means `gnu-efi` is not fully installed.
- If QEMU cannot find `/usr/share/ovmf/OVMF.fd`, install the `ovmf` package (or update `StartMiniOS` to your local firmware path).
- If UEFI does not auto-run your target, launch it manually from the shell with `run <APP>.EFI`.

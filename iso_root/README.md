# MiniOS (UEFI)

MiniOS is a tiny **x86_64 UEFI playground** built with `gnu-efi`.

The default app (`BOOTX64.EFI`) starts an interactive MiniOS UEFI shell.

On startup it prints:

`MiniOS UEFI shell`

and then shows a `MiniOS>` prompt. Use `help` to list available shell commands
(e.g. `list`, `read`, `write`, `free`, `run`, `reboot`, `halt`).

### Built-in shell commands

- `help` — show the built-in command list.
- `cls` — clear the screen and reset cursor position.
- `echo TEXT` — print `TEXT` exactly as entered.
- `cd [PATH]` — change the current working directory (uses `\` root by default).
- `list [-m] [PATH]` — list entries for a directory or file path; metadata columns are shown only with `-m`.
- `read FILE` — display the contents of `FILE`.
- `write FILE TEXT` — overwrite `FILE` with `TEXT`.
- `delete FILE` — delete a file.
- `mkdir DIR` — create a directory.
- `rmdir DIR` — remove an empty directory.
- `free` — print total, used, and free memory and disk space (MiB).
- `run EFI_FILE [ARGS]` — load and execute another EFI application, optionally with arguments.
- `APP.EFI [ARGS]` — shortcut form that auto-runs an EFI app without typing `run`.
- `APP [ARGS]` — shortcut form that appends `.EFI` automatically, then runs it.
- `edit FILE` — open `FILE` in the standalone editor (`EDIT.EFI`).
- `reboot` — reboot the machine via UEFI `ResetSystem`.
- `halt` — print a halt message and stop execution in an infinite loop.

The current build also produces standalone UEFI utilities:

- `PIX64.EFI` — computes π digits with a spigot algorithm
- `GFXTEST.EFI` — queries GOP modes and draws color bars
- `CLOCKX64.EFI` — prints the current UEFI clock time
- `GFXCLOCK.EFI` — full-screen analog clock (xclock-style) that updates continuously
- `SUNMAP.EFI` — world map demo with a real-time day/night illumination overlay; land-mask data is embedded so the EFI binary is self-contained
- `GOPQUERY.EFI` — GOP capability query tool with per-mode inspection and optional mode switching
- `EDIT.EFI` — full-screen text-mode editor for plain text files
- `VIEW.EFI` — extension-aware universal viewer (`.txt` text, `.jpg/.jpeg/.png/.bmp` image decode via stb_image, otherwise hex)

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
- `GFXCLOCK.EFI`
- `SUNMAP.EFI`
- `GOPQUERY.EFI`
- `EDIT.EFI`
- `VIEW.EFI`

`EDIT.EFI` is also copied into `iso_root/` during its build rule so it is immediately runnable in the QEMU FAT drive layout.

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
run GFXCLOCK.EFI
run SUNMAP.EFI
run GOPQUERY.EFI
run EDIT.EFI filename.txt
run VIEW.EFI file.txt
run VIEW.EFI -h file.bin
edit filename.txt
```

Because shell auto-run shortcuts are enabled, these are equivalent:

```text
PIX64.EFI
PIX64
VIEW gettysburg.txt
```

`SUNMAP.EFI` does not require external assets at runtime; the generated world land mask is embedded directly into the EFI binary.

`GOPQUERY.EFI` supports argument-based query mode:

```text
run GOPQUERY.EFI
run GOPQUERY.EFI mode 3
run GOPQUERY.EFI 3
run GOPQUERY.EFI set 2
```

## Demo programs and sample files in `iso_root/`

After `make`, all EFI apps are copied into `iso_root/` so they can be launched directly from MiniOS. The repository also includes sample data files there:

- `test.txt` — quick text file for `read`, `write`, `VIEW.EFI`, and `EDIT.EFI`
- `gettysburg.txt` — larger text sample for `read`/`edit`
- `corvette.bmp` — BMP sample for `VIEW.EFI`
- `corvette.jpg` — JPEG sample for `VIEW.EFI`
- `NvVars` — OVMF variable store file used by some local VM workflows

Useful demo commands from the MiniOS shell:

```text
read test.txt
run EDIT.EFI gettysburg.txt
run VIEW.EFI corvette.bmp
run VIEW.EFI corvette.jpg
run GOPQUERY.EFI
run GFXTEST.EFI
run GFXCLOCK.EFI
run SUNMAP.EFI
```

## Build only one app

Example for PI:

```bash
make check
make PIX64.EFI
```
## Troubleshooting

- Missing `elf_x86_64_efi.lds`, `crt0-efi-x86_64.o`, `libefi.a`, or `libgnuefi.a` means `gnu-efi` is not fully installed.
- If QEMU cannot find `/usr/share/ovmf/OVMF.fd`, install the `ovmf` package (or update `StartMiniOS` to your local firmware path).
- If UEFI does not auto-run your target, launch it manually from the shell with `run <APP>.EFI`.

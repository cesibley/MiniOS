#include <efi.h>
#include <efilib.h>

#define INPUT_MAX 256

static VOID print_prompt(VOID) {
    Print(L"\r\nMiniOS> ");
}

static INTN str_eq(CHAR16 *a, CHAR16 *b) {
    return StrCmp(a, b) == 0;
}

static INTN starts_with(CHAR16 *s, CHAR16 *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static VOID shell_help(VOID) {
    Print(L"\r\nCommands:\r\n");
    Print(L"  help        - show this help\r\n");
    Print(L"  cls         - clear screen\r\n");
    Print(L"  echo TEXT   - print TEXT\r\n");
    Print(L"  halt        - stop here forever\r\n");
}

static VOID shell_cls(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
}

static VOID shell_halt(VOID) {
    Print(L"\r\nSystem halted.\r\n");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static VOID execute_command(CHAR16 *line, EFI_SYSTEM_TABLE *SystemTable) {
    while (*line == L' ') line++;

    if (*line == 0) {
        return;
    }

    if (str_eq(line, L"help")) {
        shell_help();
        return;
    }

    if (str_eq(line, L"cls")) {
        shell_cls(SystemTable);
        return;
    }

    if (str_eq(line, L"halt")) {
        shell_halt();
        return;
    }

    if (starts_with(line, L"echo ")) {
        Print(L"\r\n%s", line + 5);
        return;
    }

    Print(L"\r\nUnknown command: %s", line);
}

static EFI_STATUS read_line(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *buffer, UINTN max_len) {
    EFI_INPUT_KEY key;
    UINTN index = 0;
    UINTN event_index;

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);

        if (uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                              SystemTable->ConIn, &key) != EFI_SUCCESS) {
            continue;
        }

        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            buffer[index] = 0;
            Print(L"\r\n");
            return EFI_SUCCESS;
        }

        if (key.UnicodeChar == CHAR_BACKSPACE) {
            if (index > 0) {
                index--;
                buffer[index] = 0;
                Print(L"\b \b");
            }
            continue;
        }

        if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
            if (index < max_len - 1) {
                buffer[index++] = key.UnicodeChar;
                buffer[index] = 0;
                Print(L"%c", key.UnicodeChar);
            }
            continue;
        }
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 line[INPUT_MAX];

    InitializeLib(ImageHandle, SystemTable);
    shell_cls(SystemTable);

    Print(L"MiniOS UEFI shell\r\n");
    Print(L"Type 'help' for commands.\r\n");

    while (1) {
        line[0] = 0;
        print_prompt();
        read_line(SystemTable, line, INPUT_MAX);
        execute_command(line, SystemTable);
    }

    return EFI_SUCCESS;
}

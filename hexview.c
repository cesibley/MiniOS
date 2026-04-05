#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 260
#define BYTES_PER_LINE 16

typedef enum {
    PAGE_NEXT_LINE,
    PAGE_NEXT_SCREEN,
    PAGE_QUIT
} page_action_t;

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINTN i;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return status;

    if (loaded_image->DeviceHandle != NULL) {
        status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                   loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
        if (!EFI_ERROR(status)) {
            return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
        }
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &FileSystemProtocol, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < handle_count; i++) {
        status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                   handles[i], &FileSystemProtocol, (VOID **)&fs);
        if (EFI_ERROR(status)) {
            continue;
        }

        status = uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
        if (!EFI_ERROR(status)) {
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
            return EFI_SUCCESS;
        }
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
    return EFI_NOT_FOUND;
}

static VOID parse_load_options_path(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16 *path, UINTN path_max) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status;
    CHAR16 *options;
    UINTN options_len;
    UINTN i = 0;
    UINTN out = 0;

    if (path_max == 0) {
        return;
    }
    path[0] = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL ||
        loaded_image->LoadOptions == NULL || loaded_image->LoadOptionsSize == 0) {
        return;
    }

    options = (CHAR16 *)loaded_image->LoadOptions;
    options_len = loaded_image->LoadOptionsSize / sizeof(CHAR16);
    if (options_len == 0) {
        return;
    }

    while (i < options_len && (options[i] == L' ' || options[i] == L'\t')) {
        i++;
    }

    if (i < options_len && options[i] == L'"') {
        i++;
        while (i < options_len && options[i] != 0 && options[i] != L'"' && out + 1 < path_max) {
            path[out++] = options[i++];
        }
    } else {
        while (i < options_len && options[i] != 0 && options[i] != L' ' && options[i] != L'\t' && out + 1 < path_max) {
            path[out++] = options[i++];
        }
    }

    path[out] = 0;
}

static CHAR16 printable_ascii(CHAR8 c) {
    if (c >= 32 && c <= 126) {
        return (CHAR16)c;
    }
    return L'.';
}

static VOID dump_line(UINT64 offset, CHAR8 *bytes, UINTN count) {
    UINTN i;

    Print(L"%08lx  ", offset);
    for (i = 0; i < BYTES_PER_LINE; i++) {
        if (i < count) {
            Print(L"%02x ", (UINTN)(UINT8)bytes[i]);
        } else {
            Print(L"   ");
        }
    }

    Print(L" |");
    for (i = 0; i < count; i++) {
        Print(L"%c", printable_ascii(bytes[i]));
    }
    Print(L"|\r\n");
}

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    Print(L"\r\nPress any key to exit...");
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
}

static page_action_t wait_for_page_action(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;
    UINTN col = 0;
    UINTN row = 0;

    if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                     SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &col, &row)) && row > 0) {
        uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, row - 1);
    }
    Print(L"-- More -- (Space: page, Enter: line, Q: quit)");

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);
        if (EFI_ERROR(uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key))) {
            continue;
        }
        if (key.UnicodeChar == L'q' || key.UnicodeChar == L'Q' || key.UnicodeChar == 0x1B) {
            return PAGE_QUIT;
        }
        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            return PAGE_NEXT_LINE;
        }
        if (key.UnicodeChar == L' ') {
            return PAGE_NEXT_SCREEN;
        }
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    CHAR16 path[INPUT_MAX];
    CHAR8 buf[BYTES_PER_LINE];
    UINT64 offset = 0;
    UINTN mode_cols = 0;
    UINTN screen_rows = 0;
    UINTN page_rows;
    UINTN lines_since_pause = 0;
    UINTN pause_after;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"HEXVIEW (UEFI)\r\n");
    parse_load_options_path(ImageHandle, SystemTable, path, INPUT_MAX);
    if (path[0] == 0) {
        Print(L"No file selected (use: hexview <file> or run HEXVIEW.EFI <file>).\r\n");
        wait_for_key(SystemTable);
        return EFI_INVALID_PARAMETER;
    }

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open filesystem: %r\r\n", status);
        wait_for_key(SystemTable);
        return status;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open '%s': %r\r\n", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        wait_for_key(SystemTable);
        return status;
    }

    Print(L"\r\nOffset    Hex bytes                                         ASCII\r\n");
    Print(L"--------  ------------------------------------------------  ----------------\r\n");
    lines_since_pause = 2;
    if (EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                    SystemTable->ConOut, SystemTable->ConOut->Mode->Mode,
                                    &mode_cols, &screen_rows)) || screen_rows < 4) {
        page_rows = 25;
    } else {
        page_rows = screen_rows;
    }
    (void)mode_cols;
    pause_after = page_rows > 1 ? page_rows - 1 : 1;

    while (1) {
        UINTN size = BYTES_PER_LINE;
        status = uefi_call_wrapper(file->Read, 3, file, &size, buf);
        if (EFI_ERROR(status)) {
            Print(L"\r\nRead error: %r\r\n", status);
            break;
        }
        if (size == 0) {
            break;
        }

        if (lines_since_pause >= pause_after) {
            page_action_t action = wait_for_page_action(SystemTable);
            if (action == PAGE_QUIT) {
                break;
            }
            if (action == PAGE_NEXT_SCREEN) {
                lines_since_pause = 0;
                uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
                uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
                Print(L"HEXVIEW (UEFI)  %s\r\n", path);
                Print(L"Offset    Hex bytes                                         ASCII\r\n");
                Print(L"--------  ------------------------------------------------  ----------------\r\n");
                lines_since_pause = 3;
            } else {
                lines_since_pause = pause_after - 1;
            }
        }

        dump_line(offset, buf, size);
        offset += size;
        lines_since_pause++;
    }

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
    wait_for_key(SystemTable);
    return EFI_SUCCESS;
}

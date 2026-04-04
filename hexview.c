#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 260
#define BYTES_PER_LINE 16

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

static EFI_STATUS read_line(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *buffer, UINTN max_len) {
    EFI_INPUT_KEY key;
    UINTN index = 0;
    UINTN event_index;

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);

        if (uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key) != EFI_SUCCESS) {
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

        if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && index < max_len - 1) {
            buffer[index++] = key.UnicodeChar;
            buffer[index] = 0;
            Print(L"%c", key.UnicodeChar);
        }
    }
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

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    CHAR16 path[INPUT_MAX];
    CHAR8 buf[BYTES_PER_LINE];
    UINT64 offset = 0;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"HEXVIEW (UEFI)\r\n");
    Print(L"File path to view: ");
    path[0] = 0;
    read_line(SystemTable, path, INPUT_MAX);
    if (path[0] == 0) {
        Print(L"No path provided.\r\n");
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
        dump_line(offset, buf, size);
        offset += size;
    }

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
    wait_for_key(SystemTable);
    return EFI_SUCCESS;
}

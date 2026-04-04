#include <efi.h>
#include <efilib.h>

#define INPUT_MAX 256
#define FILE_CHUNK 128

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

static CHAR16 *skip_spaces(CHAR16 *s) {
    while (*s == L' ') s++;
    return s;
}

static CHAR16 *mem_type_name(UINT32 type) {
    switch (type) {
        case EfiReservedMemoryType:      return L"Reserved";
        case EfiLoaderCode:              return L"LoaderCode";
        case EfiLoaderData:              return L"LoaderData";
        case EfiBootServicesCode:        return L"BootSvcCode";
        case EfiBootServicesData:        return L"BootSvcData";
        case EfiRuntimeServicesCode:     return L"RtSvcCode";
        case EfiRuntimeServicesData:     return L"RtSvcData";
        case EfiConventionalMemory:      return L"Conventional";
        case EfiUnusableMemory:          return L"Unusable";
        case EfiACPIReclaimMemory:       return L"ACPIReclaim";
        case EfiACPIMemoryNVS:           return L"ACPINVS";
        case EfiMemoryMappedIO:          return L"MMIO";
        case EfiMemoryMappedIOPortSpace: return L"MMIOPort";
        case EfiPalCode:                 return L"PalCode";
#ifdef EfiPersistentMemory
        case EfiPersistentMemory:        return L"Persistent";
#endif
        default:                         return L"Unknown";
    }
}

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
    return status;
}

static VOID shell_help(VOID) {
    Print(L"\r\nCommands:\r\n");
    Print(L"  help               - show this help\r\n");
    Print(L"  cls                - clear screen\r\n");
    Print(L"  echo TEXT          - print TEXT\r\n");
    Print(L"  list [PATH]        - list directory or file info\r\n");
    Print(L"  read FILE          - print FILE contents\r\n");
    Print(L"  write FILE TEXT    - overwrite FILE with TEXT\r\n");
    Print(L"  memmap             - show UEFI memory map\r\n");
    Print(L"  meminfo            - summarize UEFI memory by type\r\n");
    Print(L"  run EFI_FILE       - load + start another EFI application\r\n");
    Print(L"  reboot             - reboot system via UEFI ResetSystem\r\n");
    Print(L"  halt               - stop here forever\r\n");
}

static VOID shell_cls(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
}

static VOID shell_halt(VOID) {
    Print(L"\r\nSystem halted.\r\n");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static VOID shell_reboot(EFI_SYSTEM_TABLE *SystemTable) {
    Print(L"\r\nRebooting...\r\n");
    uefi_call_wrapper(SystemTable->RuntimeServices->ResetSystem, 4,
                      EfiResetCold, EFI_SUCCESS, 0, NULL);
}

static VOID shell_memmap(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_MEMORY_DESCRIPTOR *desc;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINTN i, count;
    UINT64 total_pages = 0;
    UINT64 total_bytes = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_version);

    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"\r\nGetMemoryMap probe failed: %r", status);
        return;
    }

    map_size += desc_size * 8;

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, map_size, (VOID **)&map);
    if (EFI_ERROR(status)) {
        Print(L"\r\nAllocatePool failed: %r", status);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        Print(L"\r\nGetMemoryMap failed: %r", status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
        return;
    }

    count = map_size / desc_size;

    Print(L"\r\nMemory map:");
    Print(L"\r\nIdx  Type           Pages        Start            End");

    for (i = 0; i < count; i++) {
        UINT64 start;
        UINT64 end;

        desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + (i * desc_size));
        start = desc->PhysicalStart;
        end   = desc->PhysicalStart + (desc->NumberOfPages * 4096ULL) - 1;

        Print(L"\r\n%3d  %-13s %8lx  %012lx  %012lx",
              i,
              mem_type_name(desc->Type),
              desc->NumberOfPages,
              start,
              end);

        total_pages += desc->NumberOfPages;
    }

    total_bytes = total_pages * 4096ULL;

    Print(L"\r\n\r\nDescriptors : %d", count);
    Print(L"\r\nTotal pages  : %lx", total_pages);
    Print(L"\r\nTotal bytes  : %lx", total_bytes);

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
}

static VOID shell_meminfo(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINTN i;
    UINTN count;
    UINT64 pages_by_type[16] = {0};
    UINT64 total_pages = 0;
    UINT64 usable_pages = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"\r\nGetMemoryMap probe failed: %r", status);
        return;
    }

    map_size += desc_size * 8;
    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, map_size, (VOID **)&map);
    if (EFI_ERROR(status)) {
        Print(L"\r\nAllocatePool failed: %r", status);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        Print(L"\r\nGetMemoryMap failed: %r", status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
        return;
    }

    count = map_size / desc_size;
    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
        UINT32 type = desc->Type;
        UINT64 pages = desc->NumberOfPages;

        total_pages += pages;
        if (type < 16) {
            pages_by_type[type] += pages;
        }
        if (type == EfiConventionalMemory) {
            usable_pages += pages;
        }
    }

    Print(L"\r\nMemory summary:");
    Print(L"\r\nDescriptors     : %d", count);
    Print(L"\r\nTotal memory    : %lu MiB", (total_pages * 4096ULL) / (1024ULL * 1024ULL));
    Print(L"\r\nUsable (conv.)  : %lu MiB", (usable_pages * 4096ULL) / (1024ULL * 1024ULL));
    Print(L"\r\n\r\nBy type:");
    for (i = 0; i < 16; i++) {
        if (pages_by_type[i] == 0) continue;
        Print(L"\r\n  %-13s %8lu pages (%lu MiB)",
              mem_type_name((UINT32)i),
              pages_by_type[i],
              (pages_by_type[i] * 4096ULL) / (1024ULL * 1024ULL));
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
}

static VOID print_file_info_line(EFI_FILE_INFO *info) {
    if (info->Attribute & EFI_FILE_DIRECTORY) {
        Print(L"\r\n<DIR>      %s", info->FileName);
    } else {
        Print(L"\r\n%10lu %s", info->FileSize, info->FileName);
    }
}

static VOID shell_list_path(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE handle;
    EFI_STATUS status;
    UINT8 info_buf[1024];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN size;
    CHAR16 *target = path;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    if (target == NULL || *target == 0) {
        handle = root;
    } else {
        status = uefi_call_wrapper(root->Open, 5, root, &handle, target, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status)) {
            Print(L"\r\nFailed to open '%s': %r", target, status);
            uefi_call_wrapper(root->Close, 1, root);
            return;
        }
    }

    size = sizeof(info_buf);
    status = uefi_call_wrapper(handle->GetInfo, 4, handle, &GenericFileInfo, &size, info);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to get info: %r", status);
        if (handle != root) uefi_call_wrapper(handle->Close, 1, handle);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    if (!(info->Attribute & EFI_FILE_DIRECTORY)) {
        print_file_info_line(info);
        if (handle != root) uefi_call_wrapper(handle->Close, 1, handle);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(handle->SetPosition, 2, handle, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to read directory: %r", status);
        if (handle != root) uefi_call_wrapper(handle->Close, 1, handle);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    while (1) {
        size = sizeof(info_buf);
        status = uefi_call_wrapper(handle->Read, 3, handle, &size, info);
        if (EFI_ERROR(status)) {
            Print(L"\r\nDirectory read error: %r", status);
            break;
        }
        if (size == 0) {
            break;
        }
        print_file_info_line(info);
    }

    if (handle != root) uefi_call_wrapper(handle->Close, 1, handle);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_read_file(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;
    CHAR8 chunk[FILE_CHUNK + 1];

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    Print(L"\r\n");
    while (1) {
        UINTN size = FILE_CHUNK;
        UINTN i;

        status = uefi_call_wrapper(file->Read, 3, file, &size, chunk);
        if (EFI_ERROR(status) || size == 0) {
            break;
        }

        chunk[size] = 0;
        for (i = 0; i < size; i++) {
            Print(L"%c", chunk[i]);
        }
    }

    if (EFI_ERROR(status)) {
        Print(L"\r\nRead error: %r", status);
    }

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_write_file(CHAR16 *path, CHAR16 *text, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;
    CHAR8 out[INPUT_MAX];
    UINTN len = 0;

    while (text[len] && len < (INPUT_MAX - 1)) {
        out[len] = (CHAR8)(text[len] & 0xFF);
        len++;
    }

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        status = uefi_call_wrapper(file->Delete, 1, file);
        if (EFI_ERROR(status)) {
            Print(L"\r\nFailed to replace '%s': %r", path, status);
            uefi_call_wrapper(root->Close, 1, root);
            return;
        }
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to create '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(file->Write, 3, file, &len, out);
    if (EFI_ERROR(status)) {
        Print(L"\r\nWrite failed: %r", status);
    } else {
        status = uefi_call_wrapper(file->Flush, 1, file);
        if (EFI_ERROR(status)) {
            Print(L"\r\nWrite completed but flush failed: %r", status);
        } else {
            Print(L"\r\nWrote %d bytes to '%s'", len, path);
        }
    }

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_run_file(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;
    EFI_HANDLE new_image = NULL;
    EFI_FILE_INFO *info = NULL;
    UINTN info_size = SIZE_OF_EFI_FILE_INFO + 512;
    VOID *image_buffer = NULL;
    UINTN image_size = 0;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, info_size, (VOID **)&info);
    if (EFI_ERROR(status)) {
        Print(L"\r\nAllocatePool for file info failed: %r", status);
        goto cleanup;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &info_size, info);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to get file info for '%s': %r", path, status);
        goto cleanup;
    }

    image_size = (UINTN)info->FileSize;
    if (image_size == 0) {
        Print(L"\r\n'%s' is empty.", path);
        goto cleanup;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, image_size, &image_buffer);
    if (EFI_ERROR(status)) {
        Print(L"\r\nAllocatePool for image failed: %r", status);
        goto cleanup;
    }

    status = uefi_call_wrapper(file->SetPosition, 2, file, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to seek '%s': %r", path, status);
        goto cleanup;
    }

    status = uefi_call_wrapper(file->Read, 3, file, &image_size, image_buffer);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to read '%s': %r", path, status);
        goto cleanup;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LoadImage, 6,
                               FALSE, ImageHandle, NULL, image_buffer, image_size, &new_image);
    if (EFI_ERROR(status)) {
        Print(L"\r\nLoadImage failed for '%s': %r", path, status);
        goto cleanup;
    }

    Print(L"\r\nStarting '%s'...", path);
    status = uefi_call_wrapper(SystemTable->BootServices->StartImage, 3, new_image, NULL, NULL);
    if (EFI_ERROR(status)) {
        Print(L"\r\nStartImage failed: %r", status);
        uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, new_image);
    } else {
        Print(L"\r\nReturned from '%s'.", path);
    }

cleanup:
    if (image_buffer != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, image_buffer);
    }
    if (info != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, info);
    }
    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID execute_command(CHAR16 *line, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 *arg;

    line = skip_spaces(line);

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

    if (str_eq(line, L"reboot")) {
        shell_reboot(SystemTable);
        return;
    }

    if (str_eq(line, L"memmap")) {
        shell_memmap(SystemTable);
        return;
    }

    if (str_eq(line, L"meminfo")) {
        shell_meminfo(SystemTable);
        return;
    }

    if (starts_with(line, L"echo ")) {
        Print(L"\r\n%s", line + 5);
        return;
    }

    if (starts_with(line, L"read ")) {
        arg = skip_spaces(line + 5);
        if (*arg == 0) {
            Print(L"\r\nUsage: read FILE");
            return;
        }
        shell_read_file(arg, ImageHandle, SystemTable);
        return;
    }

    if (str_eq(line, L"list")) {
        shell_list_path(NULL, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"list ")) {
        arg = skip_spaces(line + 5);
        shell_list_path(arg, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"write ")) {
        CHAR16 *path;
        CHAR16 *text;

        arg = skip_spaces(line + 6);
        if (*arg == 0) {
            Print(L"\r\nUsage: write FILE TEXT");
            return;
        }

        path = arg;
        while (*arg && *arg != L' ') arg++;
        if (*arg == 0) {
            Print(L"\r\nUsage: write FILE TEXT");
            return;
        }

        *arg = 0;
        text = skip_spaces(arg + 1);
        if (*text == 0) {
            Print(L"\r\nUsage: write FILE TEXT");
            return;
        }

        shell_write_file(path, text, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"run ")) {
        arg = skip_spaces(line + 4);
        if (*arg == 0) {
            Print(L"\r\nUsage: run EFI_FILE");
            return;
        }
        shell_run_file(arg, ImageHandle, SystemTable);
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
        execute_command(line, ImageHandle, SystemTable);
    }

    return EFI_SUCCESS;
}

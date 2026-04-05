#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 256
#define FILE_CHUNK 128

static VOID print_prompt(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->EnableCursor, 2, SystemTable->ConOut, TRUE);
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

static INTN has_efi_ext(CHAR16 *name) {
    UINTN len = StrLen(name);
    if (len < 4) return 0;

    return name[len - 4] == L'.' &&
           (name[len - 3] == L'e' || name[len - 3] == L'E') &&
           (name[len - 2] == L'f' || name[len - 2] == L'F') &&
           (name[len - 1] == L'i' || name[len - 1] == L'I');
}

static CHAR16 *skip_spaces(CHAR16 *s) {
    while (*s == L' ') s++;
    return s;
}

static INTN is_path_sep(CHAR16 c) {
    return c == L'\\' || c == L'/';
}

static VOID resolve_path(CHAR16 *cwd, CHAR16 *input, CHAR16 *out, UINTN out_len) {
    CHAR16 working[INPUT_MAX];
    CHAR16 *segments[INPUT_MAX / 2];
    UINTN seg_count = 0;
    UINTN i = 0;

    if (out_len == 0) return;
    out[0] = 0;

    if (input == NULL || *input == 0) {
        StrnCpy(out, cwd, out_len - 1);
        out[out_len - 1] = 0;
        return;
    }

    if (is_path_sep(input[0])) {
        StrnCpy(working, input, INPUT_MAX - 1);
    } else {
        SPrint(working, sizeof(working), L"%s\\%s", cwd, input);
    }
    working[INPUT_MAX - 1] = 0;

    while (working[i] != 0 && seg_count < (INPUT_MAX / 2)) {
        CHAR16 *start;
        UINTN j;

        while (is_path_sep(working[i])) i++;
        if (working[i] == 0) break;
        start = &working[i];
        j = i;
        while (working[j] != 0 && !is_path_sep(working[j])) j++;
        if (working[j] != 0) {
            working[j] = 0;
            i = j + 1;
        } else {
            i = j;
        }

        if (StrCmp(start, L".") == 0) {
            continue;
        }
        if (StrCmp(start, L"..") == 0) {
            if (seg_count > 0) seg_count--;
            continue;
        }
        segments[seg_count++] = start;
    }

    if (seg_count == 0) {
        StrnCpy(out, L"\\", out_len - 1);
        out[out_len - 1] = 0;
        return;
    }

    out[0] = L'\\';
    out[1] = 0;
    for (i = 0; i < seg_count; i++) {
        if (StrLen(out) + StrLen(segments[i]) + 2 >= out_len) {
            break;
        }
        if (i > 0 || out[1] != 0) StrCat(out, L"\\");
        StrCat(out, segments[i]);
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
    Print(L"  cd [PATH]          - change current directory\r\n");
    Print(L"  list [PATH]        - list directory or file info\r\n");
    Print(L"  read FILE          - print FILE contents\r\n");
    Print(L"  write FILE TEXT    - overwrite FILE with TEXT\r\n");
    Print(L"  del FILE           - delete a file\r\n");
    Print(L"  mkdir DIR          - create a directory\r\n");
    Print(L"  rmdir DIR          - remove an empty directory\r\n");
    Print(L"  freemem            - display total and free memory\r\n");
    Print(L"  freedisk           - display total and free disk space\r\n");
    Print(L"  run EFI_FILE [ARG] - load + start another EFI application\r\n");
    Print(L"  APP.EFI [ARG]      - shortcut for run APP.EFI [ARG]\r\n");
    Print(L"  edit FILE          - launch EDIT.EFI with FILE preloaded\r\n");
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

static VOID shell_freemem(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINTN i;
    UINTN count;
    UINT64 total_pages = 0;
    UINT64 free_pages = 0;

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
        total_pages += desc->NumberOfPages;
        if (desc->Type == EfiConventionalMemory) {
            free_pages += desc->NumberOfPages;
        }
    }

    Print(L"\r\nMemory:");
    Print(L"\r\n  Total: %lu MiB", (total_pages * 4096ULL) / (1024ULL * 1024ULL));
    Print(L"\r\n  Free : %lu MiB", (free_pages * 4096ULL) / (1024ULL * 1024ULL));

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
}

static VOID shell_freedisk(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_STATUS status;
    EFI_FILE_SYSTEM_INFO *fs_info = NULL;
    UINTN info_size = 0;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->GetInfo, 4, root, &FileSystemInfo, &info_size, fs_info);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"\r\nFailed to query filesystem info size: %r", status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, info_size, (VOID **)&fs_info);
    if (EFI_ERROR(status)) {
        Print(L"\r\nAllocatePool failed: %r", status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(root->GetInfo, 4, root, &FileSystemInfo, &info_size, fs_info);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to read filesystem info: %r", status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, fs_info);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    Print(L"\r\nDisk:");
    Print(L"\r\n  Total: %lu MiB", fs_info->VolumeSize / (1024ULL * 1024ULL));
    Print(L"\r\n  Free : %lu MiB", fs_info->FreeSpace / (1024ULL * 1024ULL));

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, fs_info);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID print_file_info_line(EFI_FILE_INFO *info) {
    EFI_TIME *modified = &info->ModificationTime;
    if (info->Attribute & EFI_FILE_DIRECTORY) {
        Print(L"\r\n<DIR>      %04d-%02d-%02d %02d:%02d %s",
              modified->Year, modified->Month, modified->Day,
              modified->Hour, modified->Minute, info->FileName);
    } else {
        Print(L"\r\n%10lu %04d-%02d-%02d %02d:%02d %s",
              info->FileSize,
              modified->Year, modified->Month, modified->Day,
              modified->Hour, modified->Minute, info->FileName);
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

static VOID shell_cd(CHAR16 *cwd, CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE handle;
    EFI_STATUS status;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN size = sizeof(info_buf);
    CHAR16 target[INPUT_MAX];

    resolve_path(cwd, path, target, INPUT_MAX);

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &handle, target, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nCannot open '%s': %r", target, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(handle->GetInfo, 4, handle, &GenericFileInfo, &size, info);
    if (EFI_ERROR(status) || !(info->Attribute & EFI_FILE_DIRECTORY)) {
        Print(L"\r\nNot a directory: %s", target);
        uefi_call_wrapper(handle->Close, 1, handle);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    StrnCpy(cwd, target, INPUT_MAX - 1);
    cwd[INPUT_MAX - 1] = 0;

    uefi_call_wrapper(handle->Close, 1, handle);
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

static VOID shell_delete_file(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(file->Delete, 1, file);
    if (EFI_ERROR(status)) {
        Print(L"\r\nDelete failed: %r", status);
    } else {
        Print(L"\r\nDeleted '%s'", path);
    }

    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_mkdir(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE dir;
    EFI_STATUS status;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &dir, path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               EFI_FILE_DIRECTORY);
    if (EFI_ERROR(status)) {
        Print(L"\r\nmkdir failed for '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    Print(L"\r\nCreated directory '%s'", path);
    uefi_call_wrapper(dir->Close, 1, dir);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_rmdir(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE dir;
    EFI_STATUS status;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &dir, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(dir->Delete, 1, dir);
    if (EFI_ERROR(status)) {
        Print(L"\r\nrmdir failed: %r", status);
    } else {
        Print(L"\r\nRemoved directory '%s'", path);
    }

    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_run_file(CHAR16 *path, CHAR16 *load_options, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_LOADED_IMAGE *new_loaded_image;
    EFI_DEVICE_PATH_PROTOCOL *file_path = NULL;
    EFI_STATUS status;
    EFI_HANDLE new_image = NULL;
    CHAR16 *child_load_options = NULL;
    UINTN load_options_bytes = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to get loaded image protocol: %r", status);
        return;
    }

    file_path = FileDevicePath(loaded_image->DeviceHandle, path);
    if (file_path == NULL) {
        Print(L"\r\nFailed to build device path for '%s'", path);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LoadImage, 6,
                               FALSE, ImageHandle, file_path, NULL, 0, &new_image);
    if (EFI_ERROR(status)) {
        Print(L"\r\nLoadImage failed for '%s': %r", path, status);
        goto cleanup;
    }

    if (load_options != NULL && *load_options != 0) {
        status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                   new_image, &LoadedImageProtocol, (VOID **)&new_loaded_image);
        if (EFI_ERROR(status)) {
            Print(L"\r\nFailed to set arguments: %r", status);
            uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, new_image);
            goto cleanup;
        }

        load_options_bytes = (StrLen(load_options) + 1) * sizeof(CHAR16);
        status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                                   EfiLoaderData, load_options_bytes, (VOID **)&child_load_options);
        if (EFI_ERROR(status)) {
            Print(L"\r\nFailed to allocate arguments: %r", status);
            uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, new_image);
            goto cleanup;
        }

        CopyMem(child_load_options, load_options, load_options_bytes);
        new_loaded_image->LoadOptions = child_load_options;
        new_loaded_image->LoadOptionsSize = (UINT32)load_options_bytes;
    }

    Print(L"\r\nStarting '%s'...", path);
    status = uefi_call_wrapper(SystemTable->BootServices->StartImage, 3, new_image, NULL, NULL);
    if (EFI_ERROR(status)) {
        Print(L"\r\nStartImage failed: %r", status);
        uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, new_image);
    } else {
        Print(L"\r\nReturned from '%s'.", path);
    }

    if (child_load_options != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, child_load_options);
    }

cleanup:
    if (file_path != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file_path);
    }
}

static VOID execute_command(CHAR16 *line, CHAR16 *cwd, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 *arg;
    CHAR16 resolved[INPUT_MAX];
    CHAR16 autorun[INPUT_MAX];

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

    if (str_eq(line, L"freemem")) {
        shell_freemem(SystemTable);
        return;
    }

    if (str_eq(line, L"freedisk")) {
        shell_freedisk(ImageHandle, SystemTable);
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
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_read_file(resolved, ImageHandle, SystemTable);
        return;
    }

    if (str_eq(line, L"cd")) {
        shell_cd(cwd, L"\\", ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"cd ")) {
        arg = skip_spaces(line + 3);
        if (*arg == 0) {
            Print(L"\r\nUsage: cd [PATH]");
            return;
        }
        shell_cd(cwd, arg, ImageHandle, SystemTable);
        return;
    }

    if (str_eq(line, L"list")) {
        shell_list_path(cwd, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"list ")) {
        arg = skip_spaces(line + 5);
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_list_path(resolved, ImageHandle, SystemTable);
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

        resolve_path(cwd, path, resolved, INPUT_MAX);
        shell_write_file(resolved, text, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"del ")) {
        arg = skip_spaces(line + 4);
        if (*arg == 0) {
            Print(L"\r\nUsage: del FILE");
            return;
        }
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_delete_file(resolved, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"mkdir ")) {
        arg = skip_spaces(line + 6);
        if (*arg == 0) {
            Print(L"\r\nUsage: mkdir DIR");
            return;
        }
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_mkdir(resolved, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"rmdir ")) {
        arg = skip_spaces(line + 6);
        if (*arg == 0) {
            Print(L"\r\nUsage: rmdir DIR");
            return;
        }
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_rmdir(resolved, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"run ")) {
        CHAR16 *app;
        CHAR16 *args;

        arg = skip_spaces(line + 4);
        if (*arg == 0) {
            Print(L"\r\nUsage: run EFI_FILE [ARGS]");
            return;
        }

        app = arg;
        while (*arg && *arg != L' ') arg++;
        if (*arg != 0) {
            *arg = 0;
            args = skip_spaces(arg + 1);
        } else {
            args = arg;
        }

        resolve_path(cwd, app, resolved, INPUT_MAX);
        shell_run_file(resolved, (*args != 0) ? args : NULL, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"edit ")) {
        CHAR16 edit_path[INPUT_MAX];

        arg = skip_spaces(line + 5);
        if (*arg == 0) {
            Print(L"\r\nUsage: edit FILE");
            return;
        }

        resolve_path(cwd, arg, edit_path, INPUT_MAX);
        StrCpy(resolved, L"\\EDIT.EFI");
        shell_run_file(resolved, edit_path, ImageHandle, SystemTable);
        return;
    }

    /*
     * Treat unknown commands as a shortcut for launching EFI apps.
     * Accept both:
     *   APP.EFI [ARGS]
     *   APP [ARGS]      (".EFI" appended automatically)
     */
    arg = line;
    while (*arg && *arg != L' ') arg++;
    if (*arg != 0) {
        *arg = 0;
        arg = skip_spaces(arg + 1);
    }

    if (line[0] != 0) {
        if (has_efi_ext(line)) {
            resolve_path(cwd, line, resolved, INPUT_MAX);
        } else {
            if (StrLen(line) + StrLen(L".EFI") + 1 > INPUT_MAX) {
                Print(L"\r\nCommand too long.");
                return;
            }
            StrnCpy(autorun, line, INPUT_MAX - 1);
            autorun[INPUT_MAX - 1] = 0;
            StrCat(autorun, L".EFI");
            resolve_path(cwd, autorun, resolved, INPUT_MAX);
        }

        shell_run_file(resolved, (*arg != 0) ? arg : NULL, ImageHandle, SystemTable);
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
    CHAR16 cwd[INPUT_MAX];

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    shell_cls(SystemTable);

    Print(L"MiniOS UEFI shell\r\n");
    Print(L"Type 'help' for commands.\r\n");
    StrCpy(cwd, L"\\");

    while (1) {
        line[0] = 0;
        print_prompt(SystemTable);
        read_line(SystemTable, line, INPUT_MAX);
        execute_command(line, cwd, ImageHandle, SystemTable);
    }

    return EFI_SUCCESS;
}

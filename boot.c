#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 256
#define FILE_CHUNK 128
#define HISTORY_SIZE 16

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

static INTN is_meta_suffix(CHAR16 *name) {
    UINTN len = StrLen(name);
    if (len < 5) return 0;

    return (name[len - 5] == L'.') &&
           (name[len - 4] == L'm' || name[len - 4] == L'M') &&
           (name[len - 3] == L'e' || name[len - 3] == L'E') &&
           (name[len - 2] == L't' || name[len - 2] == L'T') &&
           (name[len - 1] == L'a' || name[len - 1] == L'A');
}

static INTN is_hidden_meta_file(CHAR16 *name) {
    if (name == NULL || name[0] != L'.') return 0;
    return is_meta_suffix(name);
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

static CHAR16 *path_basename(CHAR16 *path) {
    CHAR16 *base = path;
    while (*path) {
        if (is_path_sep(*path)) {
            base = path + 1;
        }
        path++;
    }
    return base;
}

static VOID build_meta_path(CHAR16 *file_path, CHAR16 *meta_path, UINTN meta_path_len) {
    CHAR16 dir_part[INPUT_MAX];
    CHAR16 *base;
    UINTN i;
    INTN slash_pos = -1;
    UINTN len;

    if (meta_path_len == 0) return;
    meta_path[0] = 0;

    if (file_path == NULL || *file_path == 0) return;

    len = StrLen(file_path);
    for (i = 0; i < len; i++) {
        if (is_path_sep(file_path[i])) slash_pos = (INTN)i;
    }

    if (slash_pos <= 0) {
        StrCpy(dir_part, L"\\");
    } else {
        UINTN slash_idx = (UINTN)slash_pos;
        if (slash_idx >= INPUT_MAX) slash_idx = INPUT_MAX - 1;
        for (i = 0; i < slash_idx; i++) {
            CHAR16 c = file_path[i];
            dir_part[i] = is_path_sep(c) ? L'\\' : c;
        }
        dir_part[slash_idx] = 0;
    }

    base = path_basename(file_path);
    if (base == NULL || *base == 0) return;
    if (StrCmp(dir_part, L"\\") == 0) {
        SPrint(meta_path, meta_path_len * sizeof(CHAR16), L"\\.%s.meta", base);
    } else {
        SPrint(meta_path, meta_path_len * sizeof(CHAR16), L"%s\\.%s.meta", dir_part, base);
    }
}

static CHAR8 ascii_upcase(CHAR8 c) {
    if (c >= 'a' && c <= 'z') return (CHAR8)(c - ('a' - 'A'));
    return c;
}

static INTN ascii_starts_with_key(const CHAR8 *s, const char *key) {
    while (*key) {
        if (ascii_upcase(*s) != ascii_upcase((CHAR8)*key)) return 0;
        s++;
        key++;
    }
    return 1;
}

static VOID parse_meta_line(CHAR8 *line, CHAR8 *type_out, UINTN type_len, CHAR8 *desc_out, UINTN desc_len) {
    CHAR8 *value;
    UINTN i;

    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;

    if (ascii_starts_with_key(line, "TYPE")) {
        value = line + 4;
        while (*value == ' ' || *value == '\t' || *value == ':' || *value == '=') value++;
        for (i = 0; i + 1 < type_len && value[i] && value[i] != '\r' && value[i] != '\n'; i++) {
            type_out[i] = value[i];
        }
        type_out[i] = 0;
    } else if (ascii_starts_with_key(line, "DESC")) {
        value = line + 4;
        while (*value == ' ' || *value == '\t' || *value == ':' || *value == '=') value++;
        for (i = 0; i + 1 < desc_len && value[i] && value[i] != '\r' && value[i] != '\n'; i++) {
            desc_out[i] = value[i];
        }
        desc_out[i] = 0;
    }
}

static VOID load_meta_for_file(CHAR16 *file_path, EFI_FILE_HANDLE root, EFI_SYSTEM_TABLE *SystemTable,
                               CHAR8 *type_out, UINTN type_len, CHAR8 *desc_out, UINTN desc_len) {
    CHAR16 meta_path[INPUT_MAX];
    EFI_FILE_HANDLE meta = NULL;
    EFI_STATUS status;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN info_size = sizeof(info_buf);
    UINTN read_size;
    CHAR8 *buf = NULL;
    UINTN i;
    UINTN line_start = 0;

    if (type_len > 0) type_out[0] = 0;
    if (desc_len > 0) desc_out[0] = 0;

    if (file_path == NULL || *file_path == 0) return;
    if (is_hidden_meta_file(path_basename(file_path))) return;

    build_meta_path(file_path, meta_path, INPUT_MAX);
    if (meta_path[0] == 0) return;

    status = uefi_call_wrapper(root->Open, 5, root, &meta, meta_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return;

    status = uefi_call_wrapper(meta->GetInfo, 4, meta, &GenericFileInfo, &info_size, info);
    if (EFI_ERROR(status) || info->FileSize == 0 || info->FileSize > 4096) {
        uefi_call_wrapper(meta->Close, 1, meta);
        return;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, (UINTN)info->FileSize + 1, (VOID **)&buf);
    if (EFI_ERROR(status) || buf == NULL) {
        uefi_call_wrapper(meta->Close, 1, meta);
        return;
    }

    read_size = (UINTN)info->FileSize;
    status = uefi_call_wrapper(meta->Read, 3, meta, &read_size, buf);
    if (!EFI_ERROR(status) && read_size > 0) {
        buf[read_size] = 0;
        for (i = 0; i <= read_size; i++) {
            if (buf[i] == '\n' || buf[i] == 0) {
                CHAR8 saved = buf[i];
                buf[i] = 0;
                parse_meta_line(&buf[line_start], type_out, type_len, desc_out, desc_len);
                buf[i] = saved;
                line_start = i + 1;
            }
        }
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, buf);
    uefi_call_wrapper(meta->Close, 1, meta);
}

static VOID print_padded_name(CHAR16 *name, UINTN width) {
    UINTN i = 0;
    while (name[i] != 0 && i < width) {
        Print(L"%c", name[i]);
        i++;
    }
    while (i < width) {
        Print(L" ");
        i++;
    }
}

static VOID print_padded_ascii(CHAR8 *text, UINTN width) {
    UINTN i = 0;
    while (text != NULL && text[i] != 0 && i < width) {
        Print(L"%c", (CHAR16)text[i]);
        i++;
    }
    while (i < width) {
        Print(L" ");
        i++;
    }
}

static VOID shell_help(VOID) {
    Print(L"\r\nCommands:\r\n");
    Print(L"  help               - show this help\r\n");
    Print(L"  cls                - clear screen\r\n");
    Print(L"  echo TEXT          - print TEXT\r\n");
    Print(L"  goto [PATH]        - change current directory\r\n");
    Print(L"  list [-m] [PATH]   - list directory or file info (-m shows metadata)\r\n");
    Print(L"  read FILE          - print FILE contents\r\n");
    Print(L"  write FILE TEXT    - overwrite FILE with TEXT\r\n");
    Print(L"  delete PATH        - delete a file or empty directory\r\n");
    Print(L"  make DIR           - create a directory\r\n");
    Print(L"  make -f FILE       - create an empty file\r\n");
    Print(L"  free               - display total, used, and free memory + disk\r\n");
    Print(L"  run EFI_FILE [ARG] - load + start another EFI application\r\n");
    Print(L"  APP.EFI [ARG]      - shortcut for run APP.EFI [ARG]\r\n");
    Print(L"  reboot             - reboot system via UEFI ResetSystem\r\n");
    Print(L"  halt               - stop here forever\r\n");
    Print(L"\r\nTip: Up/Down browse history, Left/Right move cursor, Ins toggles insert/overwrite.\r\n");
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
    UINT64 used_pages = 0;
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
    used_pages = total_pages - free_pages;

    Print(L"\r\nMemory:");
    Print(L"\r\n  Total: %lu MiB", (total_pages * 4096ULL) / (1024ULL * 1024ULL));
    Print(L"\r\n  Used:  %lu MiB", (used_pages * 4096ULL) / (1024ULL * 1024ULL));
    Print(L"\r\n  Free:  %lu MiB", (free_pages * 4096ULL) / (1024ULL * 1024ULL));

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, map);
}

static VOID shell_freedisk(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_STATUS status;
    EFI_FILE_SYSTEM_INFO *fs_info = NULL;
    UINTN info_size = 0;
    UINT64 used_space = 0;

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

    used_space = fs_info->VolumeSize - fs_info->FreeSpace;

    Print(L"\r\nDisk:");
    Print(L"\r\n  Total: %lu MiB", fs_info->VolumeSize / (1024ULL * 1024ULL));
    Print(L"\r\n  Used:  %lu MiB", used_space / (1024ULL * 1024ULL));
    Print(L"\r\n  Free:  %lu MiB", fs_info->FreeSpace / (1024ULL * 1024ULL));

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, fs_info);
    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_free(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    shell_freemem(SystemTable);
    shell_freedisk(ImageHandle, SystemTable);
}

static VOID print_file_info_line(EFI_FILE_INFO *info, CHAR8 *type_meta, CHAR8 *desc_meta, INTN show_meta) {
    EFI_TIME *modified = &info->ModificationTime;
    if (info->Attribute & EFI_FILE_DIRECTORY) {
        Print(L"\r\n<DIR>      %04d-%02d-%02d %02d:%02d ",
              modified->Year, modified->Month, modified->Day,
              modified->Hour, modified->Minute);
        print_padded_name(info->FileName, 15);
    } else {
        Print(L"\r\n%10lu %04d-%02d-%02d %02d:%02d ",
              info->FileSize,
              modified->Year, modified->Month, modified->Day,
              modified->Hour, modified->Minute);
        print_padded_name(info->FileName, 15);
        if (show_meta) {
            Print(L" | ");
            print_padded_ascii(type_meta, 10);
            Print(L" | ");
            if (desc_meta != NULL && desc_meta[0] != 0) {
                Print(L"%a", desc_meta);
            }
        }
    }
}

static VOID shell_list_path(CHAR16 *path, INTN show_meta, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE handle;
    EFI_STATUS status;
    UINT8 info_buf[1024];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN size;
    CHAR16 *target = path;
    CHAR16 entry_path[INPUT_MAX];
    CHAR8 type_meta[64];
    CHAR8 desc_meta[160];

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
        if (target != NULL && is_hidden_meta_file(path_basename(target))) {
            Print(L"\r\nHidden metadata files are not shown.");
            if (handle != root) uefi_call_wrapper(handle->Close, 1, handle);
            uefi_call_wrapper(root->Close, 1, root);
            return;
        }
        if (show_meta) {
            load_meta_for_file(target, root, SystemTable, type_meta, sizeof(type_meta), desc_meta, sizeof(desc_meta));
        } else {
            type_meta[0] = 0;
            desc_meta[0] = 0;
        }
        print_file_info_line(info, type_meta, desc_meta, show_meta);
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
        if (is_hidden_meta_file(info->FileName)) {
            continue;
        }
        if (show_meta && !(info->Attribute & EFI_FILE_DIRECTORY)) {
            if (target == NULL || *target == 0 || StrCmp(target, L"\\") == 0) {
                SPrint(entry_path, sizeof(entry_path), L"\\%s", info->FileName);
            } else {
                SPrint(entry_path, sizeof(entry_path), L"%s\\%s", target, info->FileName);
            }
            load_meta_for_file(entry_path, root, SystemTable, type_meta, sizeof(type_meta), desc_meta, sizeof(desc_meta));
        } else {
            type_meta[0] = 0;
            desc_meta[0] = 0;
        }
        print_file_info_line(info, type_meta, desc_meta, show_meta);
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

static VOID shell_delete_path(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE handle;
    EFI_FILE_HANDLE meta_file;
    EFI_STATUS status;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN info_size = sizeof(info_buf);
    CHAR16 meta_path[INPUT_MAX];
    BOOLEAN is_directory = FALSE;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &handle, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    status = uefi_call_wrapper(handle->GetInfo, 4, handle, &GenericFileInfo, &info_size, info);
    if (!EFI_ERROR(status)) is_directory = (info->Attribute & EFI_FILE_DIRECTORY) != 0;

    status = uefi_call_wrapper(handle->Delete, 1, handle);
    if (EFI_ERROR(status)) {
        Print(L"\r\nDelete failed: %r", status);
    } else {
        if (is_directory) {
            Print(L"\r\nRemoved directory '%s'", path);
        } else {
            Print(L"\r\nDeleted '%s'", path);
        }
        if (!is_directory && !is_hidden_meta_file(path_basename(path))) {
            build_meta_path(path, meta_path, INPUT_MAX);
            if (meta_path[0] != 0) {
                status = uefi_call_wrapper(root->Open, 5, root, &meta_file, meta_path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                if (!EFI_ERROR(status)) {
                    status = uefi_call_wrapper(meta_file->Delete, 1, meta_file);
                    if (!EFI_ERROR(status)) {
                        Print(L"\r\nDeleted metadata '%s'", meta_path);
                    }
                }
            }
        }
    }

    uefi_call_wrapper(root->Close, 1, root);
}

static VOID shell_make(CHAR16 *path, BOOLEAN create_file, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE handle;
    EFI_STATUS status;

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &handle, path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               create_file ? 0 : EFI_FILE_DIRECTORY);
    if (EFI_ERROR(status)) {
        Print(L"\r\nmake failed for '%s': %r", path, status);
        uefi_call_wrapper(root->Close, 1, root);
        return;
    }

    if (create_file) {
        Print(L"\r\nCreated file '%s'", path);
    } else {
        Print(L"\r\nCreated directory '%s'", path);
    }
    uefi_call_wrapper(handle->Close, 1, handle);
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

    if (str_eq(line, L"free")) {
        shell_free(ImageHandle, SystemTable);
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

    if (str_eq(line, L"goto")) {
        shell_cd(cwd, L"\\", ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"goto ")) {
        arg = skip_spaces(line + 5);
        if (*arg == 0) {
            Print(L"\r\nUsage: goto [PATH]");
            return;
        }
        shell_cd(cwd, arg, ImageHandle, SystemTable);
        return;
    }

    if (str_eq(line, L"list")) {
        shell_list_path(cwd, 0, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"list ")) {
        arg = skip_spaces(line + 5);
        if (str_eq(arg, L"-m")) {
            shell_list_path(cwd, 1, ImageHandle, SystemTable);
            return;
        }
        if (starts_with(arg, L"-m ")) {
            arg = skip_spaces(arg + 2);
            resolve_path(cwd, arg, resolved, INPUT_MAX);
            shell_list_path(resolved, 1, ImageHandle, SystemTable);
            return;
        }
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_list_path(resolved, 0, ImageHandle, SystemTable);
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

    if (starts_with(line, L"delete ")) {
        arg = skip_spaces(line + 7);
        if (*arg == 0) {
            Print(L"\r\nUsage: delete PATH");
            return;
        }
        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_delete_path(resolved, ImageHandle, SystemTable);
        return;
    }

    if (starts_with(line, L"make ")) {
        BOOLEAN create_file = FALSE;

        arg = skip_spaces(line + 5);
        if (*arg == 0) {
            Print(L"\r\nUsage: make DIR");
            Print(L"\r\n       make -f FILE");
            return;
        }

        if (starts_with(arg, L"-f ")) {
            create_file = TRUE;
            arg = skip_spaces(arg + 2);
            if (*arg == 0) {
                Print(L"\r\nUsage: make -f FILE");
                return;
            }
        }

        resolve_path(cwd, arg, resolved, INPUT_MAX);
        shell_make(resolved, create_file, ImageHandle, SystemTable);
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


static VOID set_cursor_from_prompt(EFI_SYSTEM_TABLE *SystemTable,
                                   UINTN prompt_col,
                                   UINTN prompt_row,
                                   UINTN offset) {
    UINTN cols = 80;
    UINTN rows = 25;
    UINTN row;
    UINTN col;
    INT32 mode = 0;

    if (SystemTable->ConOut != NULL && SystemTable->ConOut->Mode != NULL) {
        mode = SystemTable->ConOut->Mode->Mode;
        if (mode >= 0) {
            EFI_STATUS status = uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                                  SystemTable->ConOut, mode, &cols, &rows);
            if (EFI_ERROR(status) || cols == 0) {
                cols = 80;
                rows = 25;
            }
        }
    }

    row = prompt_row + ((prompt_col + offset) / cols);
    col = (prompt_col + offset) % cols;
    if (rows > 0 && row >= rows) {
        row = rows - 1;
    }
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, col, row);
}

static VOID redraw_input(EFI_SYSTEM_TABLE *SystemTable,
                         UINTN prompt_col,
                         UINTN prompt_row,
                         UINTN old_len,
                         CHAR16 *buffer,
                         UINTN new_len,
                         UINTN cursor) {
    UINTN i;

    set_cursor_from_prompt(SystemTable, prompt_col, prompt_row, 0);
    if (new_len > 0) Print(L"%s", buffer);
    if (old_len > new_len) {
        for (i = 0; i < (old_len - new_len); i++) Print(L" ");
    }
    set_cursor_from_prompt(SystemTable, prompt_col, prompt_row, cursor);
}

static VOID history_add(CHAR16 history[HISTORY_SIZE][INPUT_MAX], UINTN *history_count, CHAR16 *line) {
    UINTN i;

    if (line == NULL || line[0] == 0) return;

    if (*history_count > 0 && StrCmp(history[*history_count - 1], line) == 0) {
        return;
    }

    if (*history_count < HISTORY_SIZE) {
        StrnCpy(history[*history_count], line, INPUT_MAX - 1);
        history[*history_count][INPUT_MAX - 1] = 0;
        (*history_count)++;
        return;
    }

    for (i = 1; i < HISTORY_SIZE; i++) {
        StrCpy(history[i - 1], history[i]);
    }

    StrnCpy(history[HISTORY_SIZE - 1], line, INPUT_MAX - 1);
    history[HISTORY_SIZE - 1][INPUT_MAX - 1] = 0;
}
static EFI_STATUS read_line(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *buffer, UINTN max_len,
                            CHAR16 history[HISTORY_SIZE][INPUT_MAX], UINTN history_count) {
    EFI_INPUT_KEY key;
    UINTN len = 0;
    UINTN cursor = 0;
    UINTN event_index;
    INTN history_index = -1;
    INTN insert_mode = 1;
    UINTN prompt_col;
    UINTN prompt_row;
    UINTN rendered_len = 0;
    CHAR16 scratch[INPUT_MAX];

    scratch[0] = 0;
    buffer[0] = 0;
    prompt_col = 0;
    prompt_row = 0;
    if (SystemTable->ConOut != NULL && SystemTable->ConOut->Mode != NULL) {
        prompt_col = (UINTN)SystemTable->ConOut->Mode->CursorColumn;
        prompt_row = (UINTN)SystemTable->ConOut->Mode->CursorRow;
    }

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);

        if (uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                              SystemTable->ConIn, &key) != EFI_SUCCESS) {
            continue;
        }

        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            buffer[len] = 0;
            Print(L"\r\n");
            return EFI_SUCCESS;
        }

        if (key.UnicodeChar == CHAR_BACKSPACE) {
            if (cursor > 0) {
                UINTN i;
                UINTN old_len = len;

                for (i = cursor - 1; i < len - 1; i++) {
                    buffer[i] = buffer[i + 1];
                }
                len--;
                cursor--;
                buffer[len] = 0;
                redraw_input(SystemTable, prompt_col, prompt_row, old_len, buffer, len, cursor);
                rendered_len = len;
            }
            continue;
        }

        if (key.ScanCode == SCAN_LEFT) {
            if (cursor > 0) {
                cursor--;
                redraw_input(SystemTable, prompt_col, prompt_row, rendered_len, buffer, len, cursor);
            }
            continue;
        }

        if (key.ScanCode == SCAN_RIGHT) {
            if (cursor < len) {
                cursor++;
                redraw_input(SystemTable, prompt_col, prompt_row, rendered_len, buffer, len, cursor);
            }
            continue;
        }

        if (key.ScanCode == SCAN_INSERT) {
            insert_mode = !insert_mode;
            continue;
        }

        if (key.ScanCode == SCAN_UP) {
            UINTN old_len = len;

            if (history_count == 0) continue;

            if (history_index < 0) {
                StrnCpy(scratch, buffer, INPUT_MAX - 1);
                scratch[INPUT_MAX - 1] = 0;
                history_index = (INTN)history_count - 1;
            } else if (history_index > 0) {
                history_index--;
            }

            StrnCpy(buffer, history[history_index], max_len - 1);
            buffer[max_len - 1] = 0;
            len = StrLen(buffer);
            cursor = len;
            redraw_input(SystemTable, prompt_col, prompt_row, old_len, buffer, len, cursor);
            rendered_len = len;
            continue;
        }

        if (key.ScanCode == SCAN_DOWN) {
            UINTN old_len = len;

            if (history_count == 0 || history_index < 0) continue;

            if (history_index < (INTN)history_count - 1) {
                history_index++;
                StrnCpy(buffer, history[history_index], max_len - 1);
                buffer[max_len - 1] = 0;
            } else {
                history_index = -1;
                StrnCpy(buffer, scratch, max_len - 1);
                buffer[max_len - 1] = 0;
            }

            len = StrLen(buffer);
            cursor = len;
            redraw_input(SystemTable, prompt_col, prompt_row, old_len, buffer, len, cursor);
            rendered_len = len;
            continue;
        }

        if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
            UINTN old_len = len;

            if (insert_mode) {
                UINTN i;
                if (len < max_len - 1) {
                    for (i = len; i > cursor; i--) {
                        buffer[i] = buffer[i - 1];
                    }
                    buffer[cursor] = key.UnicodeChar;
                    len++;
                    cursor++;
                    buffer[len] = 0;
                    redraw_input(SystemTable, prompt_col, prompt_row, old_len, buffer, len, cursor);
                    rendered_len = len;
                }
            } else {
                if (cursor < max_len - 1) {
                    if (cursor == len) {
                        len++;
                    }
                    buffer[cursor] = key.UnicodeChar;
                    cursor++;
                    buffer[len] = 0;
                    redraw_input(SystemTable, prompt_col, prompt_row, old_len, buffer, len, cursor);
                    rendered_len = len;
                }
            }
            continue;
        }
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 line[INPUT_MAX];
    CHAR16 history_line[INPUT_MAX];
    CHAR16 cwd[INPUT_MAX];
    CHAR16 history[HISTORY_SIZE][INPUT_MAX];
    UINTN history_count = 0;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    shell_cls(SystemTable);

    Print(L"MiniOS UEFI shell\r\n");
    Print(L"Type 'help' for commands.\r\n");
    StrCpy(cwd, L"\\");

    while (1) {
        line[0] = 0;
        print_prompt(SystemTable);
        read_line(SystemTable, line, INPUT_MAX, history, history_count);
        StrnCpy(history_line, line, INPUT_MAX - 1);
        history_line[INPUT_MAX - 1] = 0;
        execute_command(line, cwd, ImageHandle, SystemTable);
        history_add(history, &history_count, history_line);
    }

    return EFI_SUCCESS;
}

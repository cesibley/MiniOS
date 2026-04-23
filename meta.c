#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 260
#define TOKEN_MAX 64
#define VALUE_MAX 256
#define FILE_MAX_SIZE 8192

typedef struct {
    BOOLEAN add_mode;
    CHAR16 file_path[INPUT_MAX];
    CHAR8 key[TOKEN_MAX];
    CHAR8 value[VALUE_MAX];
} META_ARGS;

static BOOLEAN is_space(CHAR16 c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static INTN is_path_sep(CHAR16 c) {
    return c == L'\\' || c == L'/';
}

static CHAR16 upcase16(CHAR16 c) {
    if (c >= L'a' && c <= L'z') return (CHAR16)(c - (L'a' - L'A'));
    return c;
}

static BOOLEAN has_wildcards16(CHAR16 *s) {
    while (s != NULL && *s != 0) {
        if (*s == L'*' || *s == L'?') return TRUE;
        s++;
    }
    return FALSE;
}

static BOOLEAN wildcard_match_ci16(CHAR16 *pattern, CHAR16 *text) {
    while (*pattern != 0) {
        if (*pattern == L'*') {
            pattern++;
            if (*pattern == 0) return TRUE;
            while (*text != 0) {
                if (wildcard_match_ci16(pattern, text)) return TRUE;
                text++;
            }
            return wildcard_match_ci16(pattern, text);
        }
        if (*pattern == L'?') {
            if (*text == 0) return FALSE;
            pattern++;
            text++;
            continue;
        }
        if (upcase16(*pattern) != upcase16(*text)) return FALSE;
        pattern++;
        text++;
    }
    return *text == 0;
}

static BOOLEAN str_eq_ci16(CHAR16 *a, CHAR16 *b) {
    while (*a && *b) {
        if (upcase16(*a) != upcase16(*b)) return FALSE;
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static VOID normalize_path_seps(CHAR16 *path) {
    while (path != NULL && *path) {
        if (*path == L'/') *path = L'\\';
        path++;
    }
}

static CHAR16 *path_basename(CHAR16 *path) {
    CHAR16 *base = path;
    while (*path) {
        if (is_path_sep(*path)) base = path + 1;
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
        SPrint(meta_path, meta_path_len * sizeof(CHAR16), L"\\.meta\\%s.meta", base);
    } else {
        SPrint(meta_path, meta_path_len * sizeof(CHAR16), L"%s\\.meta\\%s.meta", dir_part, base);
    }
}

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) return status;

    return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

static BOOLEAN next_token(CHAR16 *opt, UINTN n, UINTN *index, CHAR16 *out, UINTN out_len) {
    UINTN i = *index;
    UINTN o = 0;

    while (i < n && is_space(opt[i])) i++;
    if (i >= n || opt[i] == 0) {
        *index = i;
        if (out_len > 0) out[0] = 0;
        return FALSE;
    }

    if (opt[i] == L'"') {
        i++;
        while (i < n && opt[i] != 0 && opt[i] != L'"' && o + 1 < out_len) {
            out[o++] = opt[i++];
        }
        if (i < n && opt[i] == L'"') i++;
    } else {
        while (i < n && opt[i] != 0 && !is_space(opt[i]) && o + 1 < out_len) {
            out[o++] = opt[i++];
        }
    }

    out[o] = 0;
    *index = i;
    return TRUE;
}

static VOID to_ascii(CHAR16 *in, CHAR8 *out, UINTN out_len) {
    UINTN i = 0;
    if (out_len == 0) return;
    while (in[i] && i + 1 < out_len) {
        out[i] = (CHAR8)(in[i] & 0xFF);
        i++;
    }
    out[i] = 0;
}

static CHAR8 to_upper8(CHAR8 c) {
    if (c >= 'a' && c <= 'z') return (CHAR8)(c - 'a' + 'A');
    return c;
}

static CHAR8 to_lower8(CHAR8 c) {
    if (c >= 'A' && c <= 'Z') return (CHAR8)(c - 'A' + 'a');
    return c;
}

static VOID normalize_key_upper(CHAR8 *key) {
    UINTN i = 0;
    if (key == NULL) return;
    while (key[i] != 0) {
        key[i] = to_upper8(key[i]);
        i++;
    }
}

static UINTN ascii_strlen(const CHAR8 *s) {
    UINTN n = 0;
    if (s == NULL) return 0;
    while (s[n] != 0) n++;
    return n;
}

static BOOLEAN split_meta_pair(CHAR16 *pair_in, CHAR8 *key_out, UINTN key_len, CHAR8 *value_out, UINTN value_len) {
    UINTN i = 0;
    UINTN sep = 0;

    while (pair_in[i] != 0) {
        if (pair_in[i] == L'=') {
            sep = i;
            break;
        }
        i++;
    }
    if (sep == 0 || pair_in[sep] == 0 || pair_in[sep + 1] == 0) return FALSE;

    {
        CHAR16 key16[TOKEN_MAX];
        CHAR16 val16[VALUE_MAX];
        UINTN k = 0;
        UINTN v = 0;
        for (i = 0; i < sep && k + 1 < TOKEN_MAX; i++) key16[k++] = pair_in[i];
        key16[k] = 0;
        i = sep + 1;
        while (pair_in[i] != 0 && v + 1 < VALUE_MAX) val16[v++] = pair_in[i++];
        val16[v] = 0;
        to_ascii(key16, key_out, key_len);
        to_ascii(val16, value_out, value_len);
    }
    return key_out[0] != 0 && value_out[0] != 0;
}

static EFI_STATUS parse_args(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, META_ARGS *args) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status;
    CHAR16 *opt;
    UINTN n;
    UINTN idx = 0;
    CHAR16 token[VALUE_MAX];

    args->add_mode = FALSE;
    args->file_path[0] = 0;
    args->key[0] = 0;
    args->value[0] = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL || loaded_image->LoadOptions == NULL ||
        loaded_image->LoadOptionsSize == 0) {
        return EFI_INVALID_PARAMETER;
    }

    opt = (CHAR16 *)loaded_image->LoadOptions;
    n = loaded_image->LoadOptionsSize / sizeof(CHAR16);

    if (!next_token(opt, n, &idx, token, INPUT_MAX)) return EFI_INVALID_PARAMETER;
    if (StrCmp(token, L"-a") == 0 || StrCmp(token, L"/a") == 0) {
        args->add_mode = TRUE;
        if (!next_token(opt, n, &idx, args->file_path, INPUT_MAX)) return EFI_INVALID_PARAMETER;
        if (!next_token(opt, n, &idx, token, VALUE_MAX)) return EFI_INVALID_PARAMETER;
        if (!split_meta_pair(token, args->key, TOKEN_MAX, args->value, VALUE_MAX)) return EFI_INVALID_PARAMETER;
        normalize_key_upper(args->key);
    } else {
        StrCpy(args->file_path, token);
    }
    normalize_path_seps(args->file_path);

    return EFI_SUCCESS;
}

static EFI_STATUS read_meta_file(EFI_FILE_HANDLE root, CHAR16 *meta_path,
                                 EFI_SYSTEM_TABLE *SystemTable,
                                 CHAR8 **buffer_out, UINTN *size_out) {
    EFI_FILE_HANDLE file = NULL;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN info_size = sizeof(info_buf);
    EFI_STATUS status;
    CHAR8 *data;
    UINTN read_size;

    *buffer_out = NULL;
    *size_out = 0;

    status = uefi_call_wrapper(root->Open, 5, root, &file, meta_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &info_size, info);
    if (EFI_ERROR(status) || info->FileSize > FILE_MAX_SIZE) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_LOAD_ERROR;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, (UINTN)info->FileSize + 1, (VOID **)&data);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    read_size = (UINTN)info->FileSize;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, data);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, data);
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    data[read_size] = 0;
    *buffer_out = data;
    *size_out = read_size;

    uefi_call_wrapper(file->Close, 1, file);
    return EFI_SUCCESS;
}

static EFI_STATUS write_meta_file(EFI_FILE_HANDLE root, CHAR16 *meta_path,
                                  CHAR8 *data, UINTN size) {
    CHAR16 dir_path[INPUT_MAX];
    UINTN i;
    EFI_FILE_HANDLE dir = NULL;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;

    StrnCpy(dir_path, meta_path, INPUT_MAX - 1);
    dir_path[INPUT_MAX - 1] = 0;
    for (i = StrLen(dir_path); i > 0; i--) {
        if (is_path_sep(dir_path[i - 1])) {
            dir_path[i - 1] = 0;
            break;
        }
    }

    if (dir_path[0] != 0) {
        status = uefi_call_wrapper(root->Open, 5, root, &dir, dir_path,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                   EFI_FILE_DIRECTORY);
        if (EFI_ERROR(status)) return status;
        uefi_call_wrapper(dir->Close, 1, dir);
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, meta_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        status = uefi_call_wrapper(file->Delete, 1, file);
        if (EFI_ERROR(status)) return status;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file, meta_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(file->Write, 3, file, &size, data);
    if (!EFI_ERROR(status)) {
        status = uefi_call_wrapper(file->Flush, 1, file);
    }

    uefi_call_wrapper(file->Close, 1, file);
    return status;
}

static BOOLEAN is_space8(CHAR8 c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static BOOLEAN line_key_matches(CHAR8 *line, UINTN line_len, CHAR8 *key, UINTN key_len) {
    UINTN i = 0;
    UINTN start;
    UINTN end;
    UINTN key_i;

    while (i < line_len && is_space8(line[i])) i++;
    if (i >= line_len) return FALSE;
    start = i;

    while (i < line_len && line[i] != '=' && line[i] != ':') i++;
    if (i >= line_len || (line[i] != '=' && line[i] != ':')) return FALSE;
    end = i;

    while (end > start && is_space8(line[end - 1])) end--;
    if (end - start != key_len) return FALSE;

    for (key_i = 0; key_i < key_len; key_i++) {
        if (to_lower8(line[start + key_i]) != to_lower8(key[key_i])) return FALSE;
    }

    return TRUE;
}

static EFI_STATUS upsert_meta(EFI_FILE_HANDLE root, CHAR16 *meta_path,
                              EFI_SYSTEM_TABLE *SystemTable,
                              CHAR8 *key, CHAR8 *value) {
    CHAR8 *old_buf = NULL;
    UINTN old_size = 0;
    EFI_STATUS status;
    CHAR8 *new_buf;
    UINTN key_len = ascii_strlen(key);
    UINTN value_len = ascii_strlen(value);
    UINTN cap;
    UINTN out = 0;
    UINTN i = 0;
    BOOLEAN updated = FALSE;

    status = read_meta_file(root, meta_path, SystemTable, &old_buf, &old_size);
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        return status;
    }

    cap = old_size + key_len + value_len + 8;
    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, cap, (VOID **)&new_buf);
    if (EFI_ERROR(status)) {
        if (old_buf != NULL) uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, old_buf);
        return status;
    }

    while (i < old_size) {
        UINTN start = i;
        UINTN end;
        while (i < old_size && old_buf[i] != '\n') i++;
        end = i;
        if (i < old_size && old_buf[i] == '\n') i++;

        if (end > start && old_buf[end - 1] == '\r') end--;
        if (end > start && line_key_matches(&old_buf[start], end - start, key, key_len)) {
            UINTN k;
            for (k = 0; k < key_len; k++) new_buf[out++] = key[k];
            new_buf[out++] = ':';
            new_buf[out++] = ' ';
            for (k = 0; k < value_len; k++) new_buf[out++] = value[k];
            new_buf[out++] = '\n';
            updated = TRUE;
        } else {
            UINTN k;
            for (k = start; k < (i <= old_size ? i : old_size); k++) new_buf[out++] = old_buf[k];
            if (i == old_size && (out == 0 || new_buf[out - 1] != '\n')) new_buf[out++] = '\n';
        }
    }

    if (!updated) {
        UINTN k;
        for (k = 0; k < key_len; k++) new_buf[out++] = key[k];
        new_buf[out++] = ':';
        new_buf[out++] = ' ';
        for (k = 0; k < value_len; k++) new_buf[out++] = value[k];
        new_buf[out++] = '\n';
    }

    status = write_meta_file(root, meta_path, new_buf, out);

    if (old_buf != NULL) uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, old_buf);
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, new_buf);
    return status;
}

static EFI_STATUS ensure_meta_for_missing_file(EFI_FILE_HANDLE root, CHAR16 *file_path) {
    EFI_FILE_HANDLE file;
    EFI_FILE_HANDLE meta_dir;
    EFI_STATUS status;
    CHAR16 dir_path[INPUT_MAX];
    CHAR16 parent[INPUT_MAX];
    UINTN i;
    UINTN slash_idx = 0;

    status = uefi_call_wrapper(root->Open, 5, root, &file, file_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) return status;
    uefi_call_wrapper(file->Close, 1, file);

    StrnCpy(dir_path, file_path, INPUT_MAX - 1);
    dir_path[INPUT_MAX - 1] = 0;
    for (i = StrLen(dir_path); i > 0; i--) {
        if (is_path_sep(dir_path[i - 1])) {
            slash_idx = i - 1;
            break;
        }
    }

    if (slash_idx == 0) {
        StrCpy(parent, L"\\");
    } else {
        for (i = 0; i < slash_idx && i + 1 < INPUT_MAX; i++) parent[i] = dir_path[i];
        parent[i] = 0;
    }

    if (str_eq_ci16(parent, L"\\")) {
        StrCpy(dir_path, L"\\.meta");
    } else {
        SPrint(dir_path, sizeof(dir_path), L"%s\\.meta", parent);
    }

    status = uefi_call_wrapper(root->Open, 5, root, &meta_dir, dir_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(status)) {
        uefi_call_wrapper(meta_dir->Close, 1, meta_dir);
    } else {
        return status;
    }
    return EFI_SUCCESS;
}

static BOOLEAN split_directory_and_pattern(CHAR16 *path_in, CHAR16 *dir_out, UINTN dir_len,
                                           CHAR16 *pattern_out, UINTN pattern_len) {
    INTN slash_pos = -1;
    UINTN i;
    UINTN path_len;

    if (path_in == NULL || *path_in == 0 || dir_len == 0 || pattern_len == 0) return FALSE;

    dir_out[0] = 0;
    pattern_out[0] = 0;
    path_len = StrLen(path_in);
    for (i = 0; i < path_len; i++) {
        if (is_path_sep(path_in[i])) slash_pos = (INTN)i;
    }

    if (slash_pos < 0) {
        StrCpy(dir_out, L"\\");
        StrnCpy(pattern_out, path_in, pattern_len - 1);
        pattern_out[pattern_len - 1] = 0;
        return pattern_out[0] != 0;
    }

    if (slash_pos == 0) {
        StrCpy(dir_out, L"\\");
    } else {
        UINTN slash_idx = (UINTN)slash_pos;
        UINTN copy_len = slash_idx;
        if (copy_len >= dir_len) copy_len = dir_len - 1;
        for (i = 0; i < copy_len; i++) {
            CHAR16 c = path_in[i];
            dir_out[i] = is_path_sep(c) ? L'\\' : c;
        }
        dir_out[copy_len] = 0;
    }

    StrnCpy(pattern_out, &path_in[slash_pos + 1], pattern_len - 1);
    pattern_out[pattern_len - 1] = 0;
    return pattern_out[0] != 0;
}

static EFI_STATUS upsert_meta_wildcard(EFI_FILE_HANDLE root, EFI_SYSTEM_TABLE *SystemTable,
                                       CHAR16 *path_pattern, CHAR8 *key, CHAR8 *value) {
    EFI_FILE_HANDLE dir = NULL;
    EFI_STATUS status;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN size;
    CHAR16 dir_path[INPUT_MAX];
    CHAR16 name_pattern[INPUT_MAX];
    CHAR16 file_path[INPUT_MAX];
    CHAR16 meta_path[INPUT_MAX];
    UINTN updated_count = 0;

    if (!split_directory_and_pattern(path_pattern, dir_path, INPUT_MAX, name_pattern, INPUT_MAX)) {
        return EFI_INVALID_PARAMETER;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &dir, dir_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(dir->SetPosition, 2, dir, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(dir->Close, 1, dir);
        return status;
    }

    while (1) {
        size = sizeof(info_buf);
        status = uefi_call_wrapper(dir->Read, 3, dir, &size, info);
        if (EFI_ERROR(status) || size == 0) break;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;
        if (!wildcard_match_ci16(name_pattern, info->FileName)) continue;

        if (str_eq_ci16(dir_path, L"\\")) {
            SPrint(file_path, sizeof(file_path), L"\\%s", info->FileName);
        } else {
            SPrint(file_path, sizeof(file_path), L"%s\\%s", dir_path, info->FileName);
        }

        build_meta_path(file_path, meta_path, INPUT_MAX);
        if (meta_path[0] == 0) continue;

        status = upsert_meta(root, meta_path, SystemTable, key, value);
        if (EFI_ERROR(status)) {
            Print(L"\r\nFailed updating '%s': %r", file_path, status);
            continue;
        }
        updated_count++;
        Print(L"\r\nUpdated '%s': %a: %a", file_path, key, value);
    }

    uefi_call_wrapper(dir->Close, 1, dir);
    if (updated_count == 0) {
        Print(L"\r\nNo matches for '%s'", path_pattern);
        return EFI_NOT_FOUND;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS resolve_existing_path_case(EFI_FILE_HANDLE root, CHAR16 *path_in,
                                             CHAR16 *path_out, UINTN path_out_len) {
    EFI_FILE_HANDLE dir = NULL;
    EFI_STATUS status;
    UINT8 info_buf[512];
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN size;
    CHAR16 dir_path[INPUT_MAX];
    CHAR16 name_part[INPUT_MAX];
    BOOLEAN found = FALSE;

    if (path_out_len == 0) return EFI_INVALID_PARAMETER;
    path_out[0] = 0;

    if (!split_directory_and_pattern(path_in, dir_path, INPUT_MAX, name_part, INPUT_MAX)) {
        return EFI_INVALID_PARAMETER;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &dir, dir_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(dir->SetPosition, 2, dir, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(dir->Close, 1, dir);
        return status;
    }

    while (1) {
        size = sizeof(info_buf);
        status = uefi_call_wrapper(dir->Read, 3, dir, &size, info);
        if (EFI_ERROR(status) || size == 0) break;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;
        if (!str_eq_ci16(name_part, info->FileName)) continue;

        if (str_eq_ci16(dir_path, L"\\")) {
            SPrint(path_out, path_out_len * sizeof(CHAR16), L"\\%s", info->FileName);
        } else {
            SPrint(path_out, path_out_len * sizeof(CHAR16), L"%s\\%s", dir_path, info->FileName);
        }
        found = TRUE;
        break;
    }

    uefi_call_wrapper(dir->Close, 1, dir);
    if (found) return EFI_SUCCESS;

    StrnCpy(path_out, path_in, path_out_len - 1);
    path_out[path_out_len - 1] = 0;
    return EFI_NOT_FOUND;
}

static VOID print_usage(VOID) {
    Print(L"\r\nUsage:\r\n");
    Print(L"  META <filename>\r\n");
    Print(L"  META -a <filename|pattern> <meta>=<text>\r\n");
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_FILE_HANDLE root;
    META_ARGS args;
    CHAR16 resolved_path[INPUT_MAX];
    CHAR16 meta_path[INPUT_MAX];

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);

    status = parse_args(ImageHandle, SystemTable, &args);
    if (EFI_ERROR(status) || args.file_path[0] == 0) {
        print_usage();
        return EFI_INVALID_PARAMETER;
    }

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"\r\nFailed to open filesystem: %r", status);
        return status;
    }

    status = resolve_existing_path_case(root, args.file_path, resolved_path, INPUT_MAX);
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        uefi_call_wrapper(root->Close, 1, root);
        Print(L"\r\nFailed to resolve path '%s': %r", args.file_path, status);
        return status;
    }
    if (!EFI_ERROR(status)) {
        StrnCpy(args.file_path, resolved_path, INPUT_MAX - 1);
        args.file_path[INPUT_MAX - 1] = 0;
    }

    build_meta_path(args.file_path, meta_path, INPUT_MAX);
    if (meta_path[0] == 0) {
        uefi_call_wrapper(root->Close, 1, root);
        Print(L"\r\nInvalid file path '%s'", args.file_path);
        return EFI_INVALID_PARAMETER;
    }

    if (args.add_mode) {
        if (has_wildcards16(args.file_path)) {
            status = upsert_meta_wildcard(root, SystemTable, args.file_path, args.key, args.value);
        } else {
            status = ensure_meta_for_missing_file(root, args.file_path);
            if (EFI_ERROR(status)) {
                Print(L"\r\nFailed to create file '%s': %r", args.file_path, status);
            } else {
                status = upsert_meta(root, meta_path, SystemTable, args.key, args.value);
                if (EFI_ERROR(status)) {
                    Print(L"\r\nFailed to update metadata '%s': %r", meta_path, status);
                } else {
                    Print(L"\r\nUpdated metadata: %a: %a", args.key, args.value);
                }
            }
        }
    } else {
        CHAR8 *buf = NULL;
        UINTN size = 0;
        status = read_meta_file(root, meta_path, SystemTable, &buf, &size);
        if (EFI_ERROR(status)) {
            Print(L"\r\nNo metadata for '%s'", args.file_path);
        } else {
            Print(L"\r\nMetadata for '%s':\r\n%a", args.file_path, buf);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, buf);
        }
    }

    uefi_call_wrapper(root->Close, 1, root);
    return status;
}

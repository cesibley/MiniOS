#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 260
#define INITIAL_LINE_CAP 8
#define INITIAL_ROW_CAP 128

typedef struct {
    CHAR16 *data;
    UINTN len;
    UINTN cap;
} line_t;

typedef struct {
    line_t *rows;
    UINTN row_count;
    UINTN row_cap;

    UINTN cx;
    UINTN cy;
    UINTN row_off;
    UINTN col_off;

    UINTN screen_cols;
    UINTN screen_rows;

    BOOLEAN dirty;
    CHAR16 status[128];
    CHAR16 path[INPUT_MAX];
    CHAR16 *render_line;
    UINTN render_cap;

    EFI_HANDLE image;
    EFI_SYSTEM_TABLE *st;
} editor_t;

static VOID set_status(editor_t *ed, CHAR16 *msg) {
    SPrint(ed->status, sizeof(ed->status), L"%s", msg);
}

static BOOLEAN key_is_save(EFI_INPUT_KEY *key) {
    return (key->ScanCode == SCAN_F2 || key->UnicodeChar == 0x13);
}

static BOOLEAN key_is_quit(EFI_INPUT_KEY *key) {
    return (key->ScanCode == SCAN_F10 || key->UnicodeChar == 0x11 || key->UnicodeChar == 0x1B);
}

static EFI_STATUS alloc_pool(editor_t *ed, UINTN size, VOID **out) {
    return uefi_call_wrapper(ed->st->BootServices->AllocatePool, 3, EfiLoaderData, size, out);
}

static VOID free_pool(editor_t *ed, VOID *ptr) {
    if (ptr != NULL) {
        uefi_call_wrapper(ed->st->BootServices->FreePool, 1, ptr);
    }
}

static EFI_STATUS ensure_line_capacity(editor_t *ed, line_t *line, UINTN needed) {
    CHAR16 *next;
    UINTN new_cap;

    if (needed <= line->cap) {
        return EFI_SUCCESS;
    }

    new_cap = line->cap == 0 ? INITIAL_LINE_CAP : line->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    if (EFI_ERROR(alloc_pool(ed, sizeof(CHAR16) * new_cap, (VOID **)&next))) {
        return EFI_OUT_OF_RESOURCES;
    }

    if (line->len > 0) {
        CopyMem(next, line->data, sizeof(CHAR16) * line->len);
    }
    free_pool(ed, line->data);
    line->data = next;
    line->cap = new_cap;
    return EFI_SUCCESS;
}

static EFI_STATUS ensure_row_capacity(editor_t *ed, UINTN needed) {
    line_t *next;
    UINTN new_cap;

    if (needed <= ed->row_cap) {
        return EFI_SUCCESS;
    }

    new_cap = ed->row_cap == 0 ? INITIAL_ROW_CAP : ed->row_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    if (EFI_ERROR(alloc_pool(ed, sizeof(line_t) * new_cap, (VOID **)&next))) {
        return EFI_OUT_OF_RESOURCES;
    }

    if (ed->row_count > 0) {
        CopyMem(next, ed->rows, sizeof(line_t) * ed->row_count);
    }
    free_pool(ed, ed->rows);
    ed->rows = next;
    ed->row_cap = new_cap;
    return EFI_SUCCESS;
}

static EFI_STATUS ensure_render_capacity(editor_t *ed, UINTN needed) {
    CHAR16 *next;
    UINTN new_cap;

    if (needed <= ed->render_cap) {
        return EFI_SUCCESS;
    }

    new_cap = ed->render_cap == 0 ? 128 : ed->render_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    if (EFI_ERROR(alloc_pool(ed, sizeof(CHAR16) * new_cap, (VOID **)&next))) {
        return EFI_OUT_OF_RESOURCES;
    }

    free_pool(ed, ed->render_line);
    ed->render_line = next;
    ed->render_cap = new_cap;
    return EFI_SUCCESS;
}

static EFI_STATUS line_insert_char(editor_t *ed, line_t *line, UINTN at, CHAR16 ch) {
    if (at > line->len) {
        at = line->len;
    }

    if (EFI_ERROR(ensure_line_capacity(ed, line, line->len + 1))) {
        return EFI_OUT_OF_RESOURCES;
    }

    if (at < line->len) {
        CopyMem(&line->data[at + 1], &line->data[at], sizeof(CHAR16) * (line->len - at));
    }
    line->data[at] = ch;
    line->len++;
    return EFI_SUCCESS;
}

static VOID line_delete_char(line_t *line, UINTN at) {
    if (at >= line->len) {
        return;
    }

    if (at + 1 < line->len) {
        CopyMem(&line->data[at], &line->data[at + 1], sizeof(CHAR16) * (line->len - at - 1));
    }
    line->len--;
}

static EFI_STATUS insert_empty_row(editor_t *ed, UINTN at) {
    UINTN i;

    if (at > ed->row_count) {
        at = ed->row_count;
    }

    if (EFI_ERROR(ensure_row_capacity(ed, ed->row_count + 1))) {
        return EFI_OUT_OF_RESOURCES;
    }

    for (i = ed->row_count; i > at; --i) {
        ed->rows[i] = ed->rows[i - 1];
    }

    ed->rows[at].data = NULL;
    ed->rows[at].len = 0;
    ed->rows[at].cap = 0;
    ed->row_count++;
    return EFI_SUCCESS;
}

static VOID delete_row(editor_t *ed, UINTN at) {
    UINTN i;

    if (at >= ed->row_count) {
        return;
    }

    free_pool(ed, ed->rows[at].data);

    for (i = at; i + 1 < ed->row_count; ++i) {
        ed->rows[i] = ed->rows[i + 1];
    }

    ed->row_count--;
}

static EFI_STATUS editor_init(editor_t *ed, EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    UINTN cols;
    UINTN rows;

    ZeroMem(ed, sizeof(*ed));
    ed->image = image;
    ed->st = st;

    status = uefi_call_wrapper(st->ConOut->QueryMode, 4, st->ConOut, st->ConOut->Mode->Mode, &cols, &rows);
    if (EFI_ERROR(status)) {
        return status;
    }
    ed->screen_cols = cols;
    ed->screen_rows = rows;

    status = insert_empty_row(ed, 0);
    if (EFI_ERROR(status)) {
        return status;
    }

    set_status(ed, L"Ready");
    return EFI_SUCCESS;
}

static VOID editor_free(editor_t *ed) {
    UINTN i;
    for (i = 0; i < ed->row_count; ++i) {
        free_pool(ed, ed->rows[i].data);
    }
    free_pool(ed, ed->rows);
    free_pool(ed, ed->render_line);
}

static VOID editor_scroll(editor_t *ed) {
    UINTN text_rows = (ed->screen_rows > 0) ? ed->screen_rows - 1 : 0;

    if (ed->cy < ed->row_off) {
        ed->row_off = ed->cy;
    }
    if (text_rows > 0 && ed->cy >= ed->row_off + text_rows) {
        ed->row_off = ed->cy - text_rows + 1;
    }

    if (ed->cx < ed->col_off) {
        ed->col_off = ed->cx;
    }
    if (ed->cx >= ed->col_off + ed->screen_cols) {
        ed->col_off = ed->cx - ed->screen_cols + 1;
    }
}

static VOID draw_spaces(UINTN count) {
    UINTN i;
    for (i = 0; i < count; ++i) {
        Print(L" ");
    }
}

static VOID editor_draw_row(editor_t *ed, UINTN y, UINTN file_row, EFI_STATUS render_status) {
    uefi_call_wrapper(ed->st->ConOut->SetCursorPosition, 3, ed->st->ConOut, 0, y);

    if (EFI_ERROR(render_status) || ed->screen_cols == 0) {
        if (file_row >= ed->row_count) {
            Print(L"~");
            if (ed->screen_cols > 1) {
                draw_spaces(ed->screen_cols - 1);
            }
            return;
        }

        {
            line_t *line = &ed->rows[file_row];
            UINTN start = ed->col_off;
            UINTN remain = 0;
            UINTN i;

            if (start < line->len) {
                remain = line->len - start;
                if (remain > ed->screen_cols) {
                    remain = ed->screen_cols;
                }

                for (i = 0; i < remain; ++i) {
                    CHAR16 ch = line->data[start + i];
                    if (ch < 32 || ch > 126) {
                        ch = L'?';
                    }
                    Print(L"%c", ch);
                }
            }

            if (ed->screen_cols > remain) {
                draw_spaces(ed->screen_cols - remain);
            }
        }
        return;
    }

    {
        UINTN x;
        for (x = 0; x < ed->screen_cols; ++x) {
            ed->render_line[x] = L' ';
        }
        ed->render_line[ed->screen_cols] = 0;
    }

    if (file_row >= ed->row_count) {
        ed->render_line[0] = L'~';
        Print(L"%s", ed->render_line);
        return;
    }

    {
        line_t *line = &ed->rows[file_row];
        UINTN start = ed->col_off;
        UINTN remain = 0;
        UINTN i;

        if (start < line->len) {
            remain = line->len - start;
            if (remain > ed->screen_cols) {
                remain = ed->screen_cols;
            }

            for (i = 0; i < remain; ++i) {
                CHAR16 ch = line->data[start + i];
                if (ch < 32 || ch > 126) {
                    ch = L'?';
                }
                ed->render_line[i] = ch;
            }
        }
    }

    Print(L"%s", ed->render_line);
}

static VOID editor_draw_rows(editor_t *ed) {
    UINTN y;
    UINTN text_rows = (ed->screen_rows > 0) ? ed->screen_rows - 1 : 0;
    EFI_STATUS render_status = EFI_SUCCESS;

    if (ed->screen_cols > 0) {
        render_status = ensure_render_capacity(ed, ed->screen_cols + 1);
    }

    for (y = 0; y < text_rows; ++y) {
        UINTN file_row = ed->row_off + y;
        editor_draw_row(ed, y, file_row, render_status);
    }
}

static VOID editor_draw_status(editor_t *ed) {
    CHAR16 bar[256];
    CHAR16 meta[192];
    UINTN col = (ed->cx + 1);
    UINTN row = (ed->cy + 1);
    UINTN used;
    UINTN i;
    UINTN status_cols = 0;
    EFI_STATUS render_status = EFI_SUCCESS;

    SPrint(meta, sizeof(meta), L"[%s]%a Ln %u, Col %u %s",
           ed->path[0] ? ed->path : L"(new)",
           ed->dirty ? " *" : "",
           row,
           col,
           ed->status);
    SPrint(bar, sizeof(bar), L"F2/Ctrl+S Save | F10/Ctrl+Q/Esc Quit | %s", meta);

    uefi_call_wrapper(ed->st->ConOut->SetCursorPosition, 3, ed->st->ConOut, 0, ed->screen_rows - 1);
    if (ed->screen_cols > 0) {
        status_cols = ed->screen_cols - 1;
        render_status = ensure_render_capacity(ed, status_cols + 1);
    }

    used = StrLen(bar);
    if (used > status_cols) {
        used = status_cols;
    }

    if (!EFI_ERROR(render_status) && status_cols > 0) {
        for (i = 0; i < status_cols; ++i) {
            ed->render_line[i] = (i < used) ? bar[i] : L' ';
        }
        ed->render_line[status_cols] = 0;
        Print(L"%s", ed->render_line);
        return;
    }

    for (i = 0; i < used; ++i) {
        Print(L"%c", bar[i]);
    }

    if (used < status_cols) {
        draw_spaces(status_cols - used);
    }
}

static VOID editor_place_cursor(editor_t *ed) {
    UINTN text_rows = (ed->screen_rows > 0) ? ed->screen_rows - 1 : 0;

    if (ed->cy >= ed->row_off && ed->cy < ed->row_off + text_rows) {
        uefi_call_wrapper(ed->st->ConOut->EnableCursor, 2, ed->st->ConOut, TRUE);
        uefi_call_wrapper(ed->st->ConOut->SetCursorPosition, 3, ed->st->ConOut,
                          ed->cx - ed->col_off, ed->cy - ed->row_off);
    } else {
        uefi_call_wrapper(ed->st->ConOut->EnableCursor, 2, ed->st->ConOut, FALSE);
    }
}

static VOID editor_refresh(editor_t *ed) {
    editor_scroll(ed);
    editor_draw_rows(ed);
    editor_draw_status(ed);
    editor_place_cursor(ed);
}

static VOID editor_refresh_cursor(editor_t *ed) {
    editor_scroll(ed);
    editor_draw_status(ed);
    editor_place_cursor(ed);
}

static VOID editor_refresh_current_row(editor_t *ed) {
    UINTN text_rows = (ed->screen_rows > 0) ? ed->screen_rows - 1 : 0;
    EFI_STATUS render_status = EFI_SUCCESS;

    editor_scroll(ed);

    if (ed->screen_cols > 0) {
        render_status = ensure_render_capacity(ed, ed->screen_cols + 1);
    }

    if (text_rows > 0 && ed->cy >= ed->row_off && ed->cy < ed->row_off + text_rows) {
        editor_draw_row(ed, ed->cy - ed->row_off, ed->cy, render_status);
    }

    editor_draw_status(ed);
    editor_place_cursor(ed);
}

static EFI_STATUS editor_insert_char(editor_t *ed, CHAR16 ch) {
    line_t *line = &ed->rows[ed->cy];
    EFI_STATUS status = line_insert_char(ed, line, ed->cx, ch);
    if (!EFI_ERROR(status)) {
        ed->cx++;
        ed->dirty = TRUE;
    }
    return status;
}

static EFI_STATUS editor_insert_newline(editor_t *ed) {
    line_t *line = &ed->rows[ed->cy];
    EFI_STATUS status;
    UINTN tail_len;

    status = insert_empty_row(ed, ed->cy + 1);
    if (EFI_ERROR(status)) {
        return status;
    }

    tail_len = line->len - ed->cx;
    if (tail_len > 0) {
        line_t *next = &ed->rows[ed->cy + 1];
        status = ensure_line_capacity(ed, next, tail_len);
        if (EFI_ERROR(status)) {
            return status;
        }
        CopyMem(next->data, &line->data[ed->cx], sizeof(CHAR16) * tail_len);
        next->len = tail_len;
        line->len = ed->cx;
    }

    ed->cy++;
    ed->cx = 0;
    ed->dirty = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS editor_backspace(editor_t *ed) {
    line_t *line;

    if (ed->cx > 0) {
        line = &ed->rows[ed->cy];
        line_delete_char(line, ed->cx - 1);
        ed->cx--;
        ed->dirty = TRUE;
        return EFI_SUCCESS;
    }

    if (ed->cy == 0) {
        return EFI_SUCCESS;
    }

    {
        line_t *prev = &ed->rows[ed->cy - 1];
        line_t *cur = &ed->rows[ed->cy];
        UINTN old_len = prev->len;

        if (EFI_ERROR(ensure_line_capacity(ed, prev, prev->len + cur->len))) {
            return EFI_OUT_OF_RESOURCES;
        }
        if (cur->len > 0) {
            CopyMem(&prev->data[prev->len], cur->data, sizeof(CHAR16) * cur->len);
        }
        prev->len += cur->len;

        delete_row(ed, ed->cy);
        ed->cy--;
        ed->cx = old_len;
        ed->dirty = TRUE;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS editor_delete(editor_t *ed) {
    line_t *line = &ed->rows[ed->cy];

    if (ed->cx < line->len) {
        line_delete_char(line, ed->cx);
        ed->dirty = TRUE;
        return EFI_SUCCESS;
    }

    if (ed->cy + 1 < ed->row_count) {
        line_t *next = &ed->rows[ed->cy + 1];
        if (EFI_ERROR(ensure_line_capacity(ed, line, line->len + next->len))) {
            return EFI_OUT_OF_RESOURCES;
        }
        if (next->len > 0) {
            CopyMem(&line->data[line->len], next->data, sizeof(CHAR16) * next->len);
        }
        line->len += next->len;
        delete_row(ed, ed->cy + 1);
        ed->dirty = TRUE;
    }

    return EFI_SUCCESS;
}

static VOID clamp_cursor_x(editor_t *ed) {
    line_t *line = &ed->rows[ed->cy];
    if (ed->cx > line->len) {
        ed->cx = line->len;
    }
}

static VOID move_cursor(editor_t *ed, UINT16 scan) {
    switch (scan) {
    case SCAN_UP:
        if (ed->cy > 0) ed->cy--;
        break;
    case SCAN_DOWN:
        if (ed->cy + 1 < ed->row_count) ed->cy++;
        break;
    case SCAN_LEFT:
        if (ed->cx > 0) {
            ed->cx--;
        } else if (ed->cy > 0) {
            ed->cy--;
            ed->cx = ed->rows[ed->cy].len;
        }
        break;
    case SCAN_RIGHT:
        if (ed->cx < ed->rows[ed->cy].len) {
            ed->cx++;
        } else if (ed->cy + 1 < ed->row_count) {
            ed->cy++;
            ed->cx = 0;
        }
        break;
    case SCAN_HOME:
        ed->cx = 0;
        break;
    case SCAN_END:
        ed->cx = ed->rows[ed->cy].len;
        break;
    case SCAN_PAGE_UP: {
        UINTN page = ed->screen_rows > 1 ? ed->screen_rows - 1 : 1;
        if (ed->cy > page) ed->cy -= page;
        else ed->cy = 0;
        break;
    }
    case SCAN_PAGE_DOWN: {
        UINTN page = ed->screen_rows > 1 ? ed->screen_rows - 1 : 1;
        ed->cy += page;
        if (ed->cy >= ed->row_count) ed->cy = ed->row_count - 1;
        break;
    }
    default:
        break;
    }

    clamp_cursor_x(ed);
}

static EFI_STATUS open_root(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINTN i;

    status = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                               image, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return status;

    if (loaded_image->DeviceHandle != NULL) {
        status = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                                   loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
        if (!EFI_ERROR(status)) {
            return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
        }
    }

    status = uefi_call_wrapper(st->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &FileSystemProtocol, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < handle_count; ++i) {
        status = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                                   handles[i], &FileSystemProtocol, (VOID **)&fs);
        if (EFI_ERROR(status)) {
            continue;
        }

        status = uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
        if (!EFI_ERROR(status)) {
            uefi_call_wrapper(st->BootServices->FreePool, 1, handles);
            return EFI_SUCCESS;
        }
    }

    uefi_call_wrapper(st->BootServices->FreePool, 1, handles);
    return EFI_NOT_FOUND;
}

static EFI_STATUS editor_load_file(editor_t *ed) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;
    UINTN chunk_cap = 4096;
    CHAR8 *buffer = NULL;
    UINTN used = 0;

    if (ed->path[0] == 0) {
        return EFI_INVALID_PARAMETER;
    }

    status = open_root(ed->image, ed->st, &root);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(root->Open, 5, root, &file, ed->path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = alloc_pool(ed, chunk_cap, (VOID **)&buffer);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    while (1) {
        UINTN read_size;
        if (used == chunk_cap) {
            CHAR8 *next;
            UINTN next_cap = chunk_cap * 2;
            status = alloc_pool(ed, next_cap, (VOID **)&next);
            if (EFI_ERROR(status)) {
                free_pool(ed, buffer);
                uefi_call_wrapper(file->Close, 1, file);
                uefi_call_wrapper(root->Close, 1, root);
                return status;
            }
            CopyMem(next, buffer, used);
            free_pool(ed, buffer);
            buffer = next;
            chunk_cap = next_cap;
        }

        read_size = chunk_cap - used;
        status = uefi_call_wrapper(file->Read, 3, file, &read_size, &buffer[used]);
        if (EFI_ERROR(status) || read_size == 0) {
            break;
        }
        used += read_size;
    }

    if (!EFI_ERROR(status)) {
        UINTN i;
        ed->row_count = 1;
        ed->rows[0].len = 0;

        ed->cx = 0;
        ed->cy = 0;

        for (i = 0; i < used; ++i) {
            CHAR8 b = buffer[i];
            CHAR16 out = 0;

            if (b == '\r') {
                continue;
            }
            if (b == '\n') {
                if (EFI_ERROR(insert_empty_row(ed, ed->cy + 1))) {
                    status = EFI_OUT_OF_RESOURCES;
                    break;
                }
                ed->cy++;
                ed->cx = 0;
                continue;
            }

            if (b == '\t') {
                out = L' ';
            } else if (b >= 32 && b <= 126) {
                out = (CHAR16)b;
            } else {
                out = L'?';
            }

            {
                line_t *line = &ed->rows[ed->cy];
                if (EFI_ERROR(ensure_line_capacity(ed, line, line->len + 1))) {
                    status = EFI_OUT_OF_RESOURCES;
                    break;
                }
                line->data[line->len++] = out;
                ed->cx = line->len;
            }
        }

        ed->cx = 0;
        ed->cy = 0;
        ed->row_off = 0;
        ed->col_off = 0;
        ed->dirty = FALSE;
    }

    free_pool(ed, buffer);
    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
    return status;
}

static EFI_STATUS editor_save_file(editor_t *ed) {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE file;
    EFI_STATUS status;
    UINTN r;

    status = open_root(ed->image, ed->st, &root);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(root->Open, 5, root, &file, ed->path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    {
        UINT64 zero = 0;
        UINTN info_size = SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16) * INPUT_MAX;
        EFI_FILE_INFO *info = NULL;

        status = alloc_pool(ed, info_size, (VOID **)&info);
        if (EFI_ERROR(status)) {
            uefi_call_wrapper(file->Close, 1, file);
            uefi_call_wrapper(root->Close, 1, root);
            return status;
        }

        status = uefi_call_wrapper(file->GetInfo, 4, file, &gEfiFileInfoGuid, &info_size, info);
        if (EFI_ERROR(status)) {
            free_pool(ed, info);
            uefi_call_wrapper(file->Close, 1, file);
            uefi_call_wrapper(root->Close, 1, root);
            return status;
        }

        info->FileSize = 0;
        info->PhysicalSize = 0;
        status = uefi_call_wrapper(file->SetInfo, 4, file, &gEfiFileInfoGuid, info_size, info);
        free_pool(ed, info);
        if (EFI_ERROR(status)) {
            uefi_call_wrapper(file->Close, 1, file);
            uefi_call_wrapper(root->Close, 1, root);
            return status;
        }

        uefi_call_wrapper(file->SetPosition, 2, file, zero);
    }

    for (r = 0; r < ed->row_count; ++r) {
        UINTN i;
        for (i = 0; i < ed->rows[r].len; ++i) {
            CHAR8 ch = (CHAR8)(ed->rows[r].data[i] & 0x7F);
            UINTN size = 1;
            status = uefi_call_wrapper(file->Write, 3, file, &size, &ch);
            if (EFI_ERROR(status)) goto done;
        }
        if (r + 1 < ed->row_count) {
            CHAR8 nl = '\n';
            UINTN size = 1;
            status = uefi_call_wrapper(file->Write, 3, file, &size, &nl);
            if (EFI_ERROR(status)) goto done;
        }
    }

    status = EFI_SUCCESS;

done:
    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
    return status;
}

static VOID parse_load_options_path(editor_t *ed) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status;
    CHAR16 *options;
    UINTN options_len;
    UINTN i = 0;
    UINTN out = 0;

    ed->path[0] = 0;
    status = uefi_call_wrapper(ed->st->BootServices->HandleProtocol, 3,
                               ed->image, &LoadedImageProtocol, (VOID **)&loaded_image);
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
        while (i < options_len && options[i] != 0 && options[i] != L'"' && out + 1 < INPUT_MAX) {
            ed->path[out++] = options[i++];
        }
    } else {
        while (i < options_len && options[i] != 0 && options[i] != L' ' && options[i] != L'\t' && out + 1 < INPUT_MAX) {
            ed->path[out++] = options[i++];
        }
    }

    ed->path[out] = 0;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    editor_t ed;
    EFI_STATUS status;
    BOOLEAN quit = FALSE;
    BOOLEAN quit_armed = FALSE;

    InitializeLib(image, st);
    disable_uefi_watchdog(st);

    status = editor_init(&ed, image, st);
    if (EFI_ERROR(status)) {
        Print(L"Editor init failed: %r\r\n", status);
        return status;
    }

    uefi_call_wrapper(st->ConOut->ClearScreen, 1, st->ConOut);
    uefi_call_wrapper(st->ConOut->EnableCursor, 2, st->ConOut, TRUE);
    Print(L"EDIT (UEFI)\r\n");
    parse_load_options_path(&ed);
    if (ed.path[0] == 0) {
        set_status(&ed, L"No file selected (use: EDIT <file>)");
    } else {
        CHAR16 load_status[64];
        status = editor_load_file(&ed);
        if (status == EFI_NOT_FOUND) {
            set_status(&ed, L"New file (F2 to save)");
        } else if (EFI_ERROR(status)) {
            SPrint(load_status, sizeof(load_status), L"Load failed: %r", status);
            set_status(&ed, load_status);
        } else {
            set_status(&ed, L"Loaded");
        }
    }

    uefi_call_wrapper(st->ConOut->ClearScreen, 1, st->ConOut);

    editor_refresh(&ed);

    while (!quit) {
        EFI_INPUT_KEY key;
        UINTN idx;

        uefi_call_wrapper(st->BootServices->WaitForEvent, 3, 1, &st->ConIn->WaitForKey, &idx);
        if (EFI_ERROR(uefi_call_wrapper(st->ConIn->ReadKeyStroke, 2, st->ConIn, &key))) {
            continue;
        }

        if (key_is_quit(&key)) {
            if (ed.dirty) {
                if (!quit_armed) {
                    quit_armed = TRUE;
                    set_status(&ed, L"Unsaved changes. Repeat quit key to discard");
                } else {
                    quit = TRUE;
                }
            } else {
                quit = TRUE;
            }
            continue;
        }
        quit_armed = FALSE;

        if (key_is_save(&key)) {
            if (ed.path[0] == 0) {
                set_status(&ed, L"No path selected");
            } else {
                status = editor_save_file(&ed);
                if (EFI_ERROR(status)) {
                    set_status(&ed, L"Save failed");
                } else {
                    ed.dirty = FALSE;
                    set_status(&ed, L"Saved");
                }
            }
            continue;
        }

        if (key.ScanCode == SCAN_DELETE) {
            UINTN prev_row_off = ed.row_off;
            UINTN prev_col_off = ed.col_off;
            UINTN prev_row_count = ed.row_count;
            UINTN prev_cy = ed.cy;
            if (EFI_ERROR(editor_delete(&ed))) {
                set_status(&ed, L"Out of memory");
            }
            editor_scroll(&ed);
            if (prev_row_off != ed.row_off || prev_col_off != ed.col_off ||
                prev_row_count != ed.row_count || prev_cy != ed.cy) {
                editor_refresh(&ed);
            } else {
                editor_refresh_current_row(&ed);
            }
            continue;
        }

        if (key.ScanCode != SCAN_NULL) {
            UINTN prev_row_off = ed.row_off;
            UINTN prev_col_off = ed.col_off;
            move_cursor(&ed, key.ScanCode);
            editor_scroll(&ed);
            if (prev_row_off != ed.row_off || prev_col_off != ed.col_off) {
                editor_refresh(&ed);
            } else {
                editor_refresh_cursor(&ed);
            }
            continue;
        }

        {
            UINTN prev_row_off = ed.row_off;
            UINTN prev_col_off = ed.col_off;
            UINTN prev_row_count = ed.row_count;
            UINTN prev_cy = ed.cy;
            BOOLEAN changed = FALSE;

            if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
                if (EFI_ERROR(editor_insert_newline(&ed))) {
                    set_status(&ed, L"Out of memory");
                } else {
                    changed = TRUE;
                }
            } else if (key.UnicodeChar == CHAR_BACKSPACE) {
                if (EFI_ERROR(editor_backspace(&ed))) {
                    set_status(&ed, L"Out of memory");
                } else {
                    changed = TRUE;
                }
            } else if (key.UnicodeChar == CHAR_TAB) {
                UINTN n;
                for (n = 0; n < 4; ++n) {
                    if (EFI_ERROR(editor_insert_char(&ed, L' '))) {
                        set_status(&ed, L"Out of memory");
                        break;
                    }
                    changed = TRUE;
                }
            } else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126) {
                if (EFI_ERROR(editor_insert_char(&ed, key.UnicodeChar))) {
                    set_status(&ed, L"Out of memory");
                } else {
                    changed = TRUE;
                }
            }

            if (changed) {
                editor_scroll(&ed);
                if (prev_row_off != ed.row_off || prev_col_off != ed.col_off ||
                    prev_row_count != ed.row_count || prev_cy != ed.cy) {
                    editor_refresh(&ed);
                } else {
                    editor_refresh_current_row(&ed);
                }
            } else {
                editor_refresh_cursor(&ed);
            }
        }
    }

    uefi_call_wrapper(st->ConOut->ClearScreen, 1, st->ConOut);
    uefi_call_wrapper(st->ConOut->EnableCursor, 2, st->ConOut, TRUE);
    uefi_call_wrapper(st->ConOut->SetCursorPosition, 3, st->ConOut, 0, 0);
    editor_free(&ed);
    return EFI_SUCCESS;
}

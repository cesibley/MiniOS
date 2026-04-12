#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz) AllocatePool((UINTN)(sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) ReallocatePool((p), (UINTN)(oldsz), (UINTN)(newsz))
#define STBI_FREE(p) FreePool((p))
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define INPUT_MAX 260
#define BYTES_PER_LINE 16

typedef enum {
    VIEW_TEXT,
    VIEW_IMAGE,
    VIEW_HEX
} view_mode_t;

typedef enum {
    PAGE_NEXT_LINE,
    PAGE_NEXT_SCREEN,
    PAGE_QUIT
} page_action_t;

static UINT16 rd_u16(const UINT8 *p) {
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static UINT32 rd_u32(const UINT8 *p) {
    return (UINT32)p[0] |
           ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}

static INT32 rd_s32(const UINT8 *p) {
    return (INT32)rd_u32(p);
}

static EFI_STATUS decode_bmp_rgb(const UINT8 *data, UINTN size,
                                 UINT8 **rgba_out, UINTN *w_out, UINTN *h_out) {
    UINT32 pixel_offset;
    UINT32 dib_size;
    INT32 width;
    INT32 height;
    UINT16 planes;
    UINT16 bpp;
    UINT32 compression;
    UINTN abs_h;
    UINTN row_stride;
    UINTN x;
    UINTN y;
    UINT8 *rgba;

    if (size < 54) return EFI_LOAD_ERROR;
    if (!(data[0] == 'B' && data[1] == 'M')) return EFI_UNSUPPORTED;

    pixel_offset = rd_u32(data + 10);
    dib_size = rd_u32(data + 14);
    if (dib_size < 40 || 14 + dib_size > size) return EFI_UNSUPPORTED;

    width = rd_s32(data + 18);
    height = rd_s32(data + 22);
    planes = rd_u16(data + 26);
    bpp = rd_u16(data + 28);
    compression = rd_u32(data + 30);

    if (width <= 0 || height == 0 || planes != 1) return EFI_UNSUPPORTED;
    if (compression != 0) return EFI_UNSUPPORTED;
    if (!(bpp == 24 || bpp == 32)) return EFI_UNSUPPORTED;

    abs_h = (height < 0) ? (UINTN)(-height) : (UINTN)height;
    row_stride = (((UINTN)width * (UINTN)bpp + 31U) / 32U) * 4U;
    if (pixel_offset >= size || row_stride == 0) return EFI_LOAD_ERROR;
    if ((UINTN)pixel_offset + row_stride * abs_h > size) return EFI_LOAD_ERROR;

    rgba = AllocatePool((UINTN)width * abs_h * 4U);
    if (rgba == NULL) return EFI_OUT_OF_RESOURCES;

    for (y = 0; y < abs_h; y++) {
        UINTN src_y = (height > 0) ? (abs_h - 1U - y) : y;
        const UINT8 *src_row = data + pixel_offset + src_y * row_stride;
        UINT8 *dst_row = rgba + y * (UINTN)width * 4U;
        for (x = 0; x < (UINTN)width; x++) {
            const UINT8 *src = src_row + x * (bpp / 8U);
            UINT8 *dst = dst_row + x * 4U;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = (bpp == 32) ? src[3] : 0xFF;
        }
    }

    *rgba_out = rgba;
    *w_out = (UINTN)width;
    *h_out = abs_h;
    return EFI_SUCCESS;
}

static BOOLEAN is_space(CHAR16 c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static INTN chricmp(CHAR16 a, CHAR16 b) {
    if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
    if (b >= L'A' && b <= L'Z') b = (CHAR16)(b - L'A' + L'a');
    return (INTN)a - (INTN)b;
}

static BOOLEAN ends_with_icase(CONST CHAR16 *s, CONST CHAR16 *suffix) {
    UINTN sl = StrLen((CHAR16 *)s);
    UINTN tl = StrLen((CHAR16 *)suffix);
    UINTN i;
    if (tl > sl) return FALSE;
    for (i = 0; i < tl; i++) {
        if (chricmp(s[sl - tl + i], suffix[i]) != 0) return FALSE;
    }
    return TRUE;
}

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL || loaded_image->DeviceHandle == NULL) {
        return EFI_NOT_FOUND;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        return status;
    }

    return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

static VOID parse_args(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable,
                       BOOLEAN *force_hex, CHAR16 *path, UINTN path_max) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status;
    CHAR16 *opt;
    UINTN n, i = 0;
    CHAR16 token0[16];
    UINTN t0 = 0;

    *force_hex = FALSE;
    path[0] = 0;
    token0[0] = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL || loaded_image->LoadOptions == NULL ||
        loaded_image->LoadOptionsSize == 0) {
        return;
    }

    opt = (CHAR16 *)loaded_image->LoadOptions;
    n = loaded_image->LoadOptionsSize / sizeof(CHAR16);

    while (i < n && is_space(opt[i])) i++;
    if (i < n && opt[i] == L'"') {
        i++;
        while (i < n && opt[i] != 0 && opt[i] != L'"' && t0 + 1 < sizeof(token0) / sizeof(token0[0])) {
            token0[t0++] = opt[i++];
        }
        token0[t0] = 0;
        if (i < n && opt[i] == L'"') i++;
    } else {
        while (i < n && opt[i] != 0 && !is_space(opt[i]) && t0 + 1 < sizeof(token0) / sizeof(token0[0])) {
            token0[t0++] = opt[i++];
        }
        token0[t0] = 0;
    }

    if (StrCmp(token0, L"-h") == 0 || StrCmp(token0, L"/h") == 0) {
        *force_hex = TRUE;
        while (i < n && is_space(opt[i])) i++;
    } else {
        i = 0;
        while (i < n && is_space(opt[i])) i++;
    }

    if (i >= n || opt[i] == 0) return;

    {
        UINTN out = 0;
        if (opt[i] == L'"') {
            i++;
            while (i < n && opt[i] != 0 && opt[i] != L'"' && out + 1 < path_max) {
                path[out++] = opt[i++];
            }
        } else {
            while (i < n && opt[i] != 0 && !is_space(opt[i]) && out + 1 < path_max) {
                path[out++] = opt[i++];
            }
        }
        path[out] = 0;
    }
}

static EFI_STATUS read_file(EFI_FILE_HANDLE root, CONST CHAR16 *path, UINT8 **data_out, UINTN *size_out) {
    EFI_FILE_HANDLE file;
    EFI_FILE_INFO *info = NULL;
    EFI_STATUS status;
    UINTN info_size = 0;
    UINTN read_size;
    UINTN expected_size;
    UINT8 *data;

    status = uefi_call_wrapper(root->Open, 5, root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_LOAD_ERROR;
    }

    info = AllocatePool(info_size);
    if (info == NULL) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_OUT_OF_RESOURCES;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file, &GenericFileInfo, &info_size, info);
    if (EFI_ERROR(status)) {
        FreePool(info);
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    if (info->FileSize > 64U * 1024U * 1024U) {
        FreePool(info);
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_BAD_BUFFER_SIZE;
    }

    expected_size = (UINTN)info->FileSize;
    data = AllocatePool(expected_size == 0 ? 1 : expected_size);
    if (data == NULL) {
        FreePool(info);
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_OUT_OF_RESOURCES;
    }

    read_size = expected_size;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, data);
    FreePool(info);
    uefi_call_wrapper(file->Close, 1, file);

    if (EFI_ERROR(status) || read_size != expected_size) {
        FreePool(data);
        return EFI_LOAD_ERROR;
    }

    *data_out = data;
    *size_out = read_size;
    return EFI_SUCCESS;
}

static page_action_t wait_for_page_action(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;
    UINTN col = 0, row = 0;

    if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                     SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &col, &row)) && row > 0) {
        uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, row - 1);
    }
    Print(L"-- More -- (Space: page, Enter: line, F10: quit)");

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3, 1, &SystemTable->ConIn->WaitForKey, &event_index);
        if (EFI_ERROR(uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key))) continue;
        if (key.ScanCode == SCAN_F10) return PAGE_QUIT;
        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) return PAGE_NEXT_LINE;
        if (key.UnicodeChar == L' ') return PAGE_NEXT_SCREEN;
    }
}

static VOID wait_for_f10(EFI_SYSTEM_TABLE *SystemTable, CONST CHAR16 *prompt) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    if (prompt != NULL) Print(L"\r\n%s", prompt);
    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3, 1, &SystemTable->ConIn->WaitForKey, &event_index);
        if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key)) &&
            key.ScanCode == SCAN_F10) {
            return;
        }
    }
}

static VOID wait_for_f10_bottom(EFI_SYSTEM_TABLE *SystemTable, CONST CHAR16 *prompt) {
    UINTN cols = 0;
    UINTN rows = 0;

    if (prompt != NULL &&
        !EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                     SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &cols, &rows)) &&
        rows > 0) {
        uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, rows - 1);
        Print(L"%s", prompt);
    }
    wait_for_f10(SystemTable, NULL);
}

static VOID dump_hex_line(UINT64 offset, CONST UINT8 *bytes, UINTN count) {
    UINTN i;
    Print(L"%08lx  ", offset);
    for (i = 0; i < BYTES_PER_LINE; i++) {
        if (i < count) Print(L"%02x ", (UINTN)bytes[i]);
        else Print(L"   ");
    }
    Print(L" |");
    for (i = 0; i < count; i++) {
        CHAR8 c = (CHAR8)bytes[i];
        Print(L"%c", (c >= 32 && c <= 126) ? c : '.');
    }
    Print(L"|\r\n");
}

static VOID view_hex(EFI_SYSTEM_TABLE *SystemTable, CONST UINT8 *data, UINTN size) {
    UINTN cols = 0, rows = 0;
    UINTN page_rows = 25, pause_after;
    UINTN lines_since_pause = 2;
    UINTN i;

    Print(L"\r\nOffset    Hex bytes                                         ASCII\r\n");
    Print(L"--------  ------------------------------------------------  ----------------\r\n");
    if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                     SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &cols, &rows)) && rows >= 4) {
        page_rows = rows;
    }
    pause_after = page_rows > 1 ? page_rows - 1 : 1;

    for (i = 0; i < size; i += BYTES_PER_LINE) {
        UINTN count = (size - i > BYTES_PER_LINE) ? BYTES_PER_LINE : (size - i);
        if (lines_since_pause >= pause_after) {
            page_action_t action = wait_for_page_action(SystemTable);
            if (action == PAGE_QUIT) return;
            if (action == PAGE_NEXT_SCREEN) {
                lines_since_pause = 0;
                uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
                uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
            } else if (lines_since_pause > 0) {
                lines_since_pause--;
            }
        }
        dump_hex_line((UINT64)i, data + i, count);
        lines_since_pause++;
    }

    wait_for_f10(SystemTable, L"Press F10 to exit...");
}

static VOID view_text(EFI_SYSTEM_TABLE *SystemTable, CONST UINT8 *data, UINTN size) {
    UINTN cols = 0, rows = 0, page_rows = 25, pause_after;
    UINTN lines_since_pause = 0;
    UINTN i;

    if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConOut->QueryMode, 4,
                                     SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &cols, &rows)) && rows >= 4) {
        page_rows = rows;
    }
    pause_after = page_rows > 1 ? page_rows - 1 : 1;

    Print(L"\r\n");
    for (i = 0; i < size; i++) {
        CHAR8 c = (CHAR8)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            Print(L"\r\n");
            lines_since_pause++;
        } else if (c >= 32 && c <= 126) {
            Print(L"%c", c);
        } else if (c == '\t') {
            Print(L"    ");
        } else {
            Print(L".");
        }

        if (lines_since_pause >= pause_after) {
            page_action_t action = wait_for_page_action(SystemTable);
            if (action == PAGE_QUIT) return;
            if (action == PAGE_NEXT_SCREEN) {
                lines_since_pause = 0;
                uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
                uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
            } else if (lines_since_pause > 0) {
                lines_since_pause--;
            }
        }
    }

    wait_for_f10(SystemTable, L"Press F10 to exit...");
}

static EFI_STATUS draw_image_fit(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                 const UINT8 *rgba, UINTN src_w, UINTN src_h) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = {0, 0, 0, 0};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *row;
    UINTN screen_w = gop->Mode->Info->HorizontalResolution;
    UINTN screen_h = gop->Mode->Info->VerticalResolution;
    UINTN draw_w, draw_h, off_x, off_y, x, y;

    if (src_w == 0 || src_h == 0) return EFI_INVALID_PARAMETER;

    draw_w = screen_w;
    draw_h = (screen_w * src_h) / src_w;
    if (draw_h > screen_h) {
        draw_h = screen_h;
        draw_w = (screen_h * src_w) / src_h;
    }
    if (draw_w == 0) draw_w = 1;
    if (draw_h == 0) draw_h = 1;

    off_x = (screen_w - draw_w) / 2U;
    off_y = (screen_h - draw_h) / 2U;

    uefi_call_wrapper(gop->Blt, 10, gop, &black, EfiBltVideoFill, 0, 0, 0, 0, screen_w, screen_h, 0);

    row = AllocatePool(sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * draw_w);
    if (row == NULL) return EFI_OUT_OF_RESOURCES;

    for (y = 0; y < draw_h; y++) {
        UINTN src_y = (y * src_h) / draw_h;
        const UINT8 *src_row = rgba + src_y * src_w * 4U;
        for (x = 0; x < draw_w; x++) {
            UINTN src_x = (x * src_w) / draw_w;
            const UINT8 *p = src_row + src_x * 4U;
            row[x].Red = p[0]; row[x].Green = p[1]; row[x].Blue = p[2]; row[x].Reserved = 0;
        }
        uefi_call_wrapper(gop->Blt, 10, gop, row, EfiBltBufferToVideo,
                          0, 0, off_x, off_y + y, draw_w, 1,
                          sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * draw_w);
    }

    FreePool(row);
    return EFI_SUCCESS;
}

static EFI_STATUS view_image(EFI_SYSTEM_TABLE *SystemTable, CONST UINT8 *data, UINTN size) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;
    UINT8 *rgba = NULL;
    UINTN w = 0, h = 0;
    int iw = 0, ih = 0;
    BOOLEAN used_stbi = FALSE;

    if (size == 0 || size > 0x7fffffffU) return EFI_BAD_BUFFER_SIZE;

    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        status = decode_bmp_rgb(data, size, &rgba, &w, &h);
        if (EFI_ERROR(status)) return status;
    } else {
        rgba = (UINT8 *)stbi_load_from_memory((const stbi_uc *)data, (int)size, &iw, &ih, NULL, 4);
        if (rgba == NULL || iw <= 0 || ih <= 0) return EFI_UNSUPPORTED;
        w = (UINTN)iw;
        h = (UINTN)ih;
        used_stbi = TRUE;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        if (used_stbi) stbi_image_free(rgba);
        else FreePool(rgba);
        return status;
    }

    status = draw_image_fit(gop, rgba, w, h);
    if (used_stbi) stbi_image_free(rgba);
    else FreePool(rgba);
    if (EFI_ERROR(status)) return status;

    wait_for_f10_bottom(SystemTable, L"Press F10 to exit image view...");
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_FILE_HANDLE root = NULL;
    CHAR16 path[INPUT_MAX];
    BOOLEAN force_hex = FALSE;
    UINT8 *file_data = NULL;
    UINTN file_size = 0;
    view_mode_t mode;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"VIEW (UEFI)\r\n");
    parse_args(ImageHandle, SystemTable, &force_hex, path, INPUT_MAX);
    if (path[0] == 0) {
        Print(L"Usage: VIEW.EFI [-h] <file>\r\n");
        wait_for_f10(SystemTable, L"Press F10 to exit...");
        return EFI_INVALID_PARAMETER;
    }

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open filesystem: %r\r\n", status);
        wait_for_f10(SystemTable, L"Press F10 to exit...");
        return status;
    }

    status = read_file(root, path, &file_data, &file_size);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read '%s': %r\r\n", path, status);
        wait_for_f10(SystemTable, L"Press F10 to exit...");
        return status;
    }

    if (force_hex) {
        mode = VIEW_HEX;
    } else if (ends_with_icase(path, L".txt")) {
        mode = VIEW_TEXT;
    } else if (ends_with_icase(path, L".jpg") || ends_with_icase(path, L".jpeg") ||
               ends_with_icase(path, L".png") || ends_with_icase(path, L".bmp")) {
        mode = VIEW_IMAGE;
    } else {
        mode = VIEW_HEX;
    }

    if (mode == VIEW_TEXT) {
        Print(L"Mode: text\r\n");
        view_text(SystemTable, file_data, file_size);
    } else if (mode == VIEW_IMAGE) {
        status = view_image(SystemTable, file_data, file_size);
        if (EFI_ERROR(status)) {
            uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
            uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
            Print(L"Image decode/display failed, falling back to hex: %r\r\n", status);
            view_hex(SystemTable, file_data, file_size);
        }
    } else {
        Print(L"Mode: hex\r\n");
        view_hex(SystemTable, file_data, file_size);
    }

    FreePool(file_data);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
    return EFI_SUCCESS;
}

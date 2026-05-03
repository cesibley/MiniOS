#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

static int tt_ifloor(double x) {
    int i = (int)x;
    return (x < (double)i) ? (i - 1) : i;
}

static int tt_iceil(double x) {
    int i = (int)x;
    return (x > (double)i) ? (i + 1) : i;
}

#define STBTT_ifloor(x) tt_ifloor((x))
#define STBTT_iceil(x)  tt_iceil((x))
#define STBTT_sqrt(x)   (x)
#define STBTT_pow(x,y)  (x)
#define STBTT_fmod(x,y) (0.0)
#define STBTT_cos(x)    (1.0)
#define STBTT_acos(x)   (0.0)
#define STBTT_fabs(x)   (((x) < 0) ? -(x) : (x))

#define STBTT_malloc(sz, u) AllocatePool((UINTN)(sz))
#define STBTT_free(p, u) FreePool((p))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static VOID clear_screen(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
}

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
}

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_STATUS status;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL || loaded_image->DeviceHandle == NULL) return EFI_NOT_FOUND;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status) || fs == NULL) return EFI_NOT_FOUND;

    return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

static EFI_STATUS read_file(EFI_FILE_HANDLE root, CONST CHAR16 *path, UINT8 **data_out, UINTN *size_out) {
    EFI_FILE_HANDLE file = NULL;
    EFI_FILE_INFO *info = NULL;
    UINTN info_size = 0;
    EFI_STATUS status;
    UINT8 *data;
    UINTN read_size;

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

    data = AllocatePool((UINTN)info->FileSize);
    if (data == NULL) {
        FreePool(info);
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_OUT_OF_RESOURCES;
    }

    read_size = (UINTN)info->FileSize;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, data);
    FreePool(info);
    uefi_call_wrapper(file->Close, 1, file);

    if (EFI_ERROR(status) || read_size == 0) {
        FreePool(data);
        return EFI_LOAD_ERROR;
    }

    *data_out = data;
    *size_out = read_size;
    return EFI_SUCCESS;
}

static EFI_STATUS render_bitmap_with_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                         unsigned char *bitmap,
                                         int width,
                                         int height,
                                         int dst_x,
                                         int dst_y) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *blt;
    UINTN x, y;

    blt = AllocatePool((UINTN)width * (UINTN)height * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    if (blt == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    for (y = 0; y < (UINTN)height; y++) {
        for (x = 0; x < (UINTN)width; x++) {
            UINT8 p = bitmap[y * (UINTN)width + x];
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL px;
            px.Blue = p;
            px.Green = p;
            px.Red = p;
            px.Reserved = 0;
            blt[y * (UINTN)width + x] = px;
        }
    }

    status = uefi_call_wrapper(gop->Blt, 10,
                               gop,
                               blt,
                               EfiBltBufferToVideo,
                               0, 0,
                               (UINTN)dst_x, (UINTN)dst_y,
                               (UINTN)width, (UINTN)height,
                               (UINTN)width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

    FreePool(blt);
    return status;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE_HANDLE root = NULL;
    UINT8 *font_data = NULL;
    UINTN font_size = 0;
    EFI_STATUS status;
    stbtt_fontinfo font;
    float scale;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    const CHAR16 *font_path = L"\\Launcher Icons\\DejaVuSans.ttf";
    const char *line1 = "ABC";
    const char *line2 = "abc";

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    clear_screen(SystemTable);

    Print(L"stb_truetype UEFI sample\r\n");

    status = open_root(ImageHandle, SystemTable, &root);
    Print(L"[2] open_root status=%r\r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"open_root failed: %r\r\n", status);
        return status;
    }

    status = read_file(root, font_path, &font_data, &font_size);
    Print(L"[3] read_file status=%r size=%lu\r\n", status, font_size);
    if (EFI_ERROR(status)) {
        Print(L"read_file(%s) failed: %r\r\n", font_path, status);
        return status;
    }

    if (!stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
        Print(L"stbtt_InitFont failed\r\n");
        FreePool(font_data);
        return EFI_LOAD_ERROR;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        Print(L"Locate GOP failed: %r\r\n", status);
        FreePool(font_data);
        return EFI_UNSUPPORTED;
    }

    scale = stbtt_ScaleForPixelHeight(&font, 72.0f);

    {
        const char *lines[2] = { line1, line2 };
        int line;
        int ascent = 0, descent = 0, line_gap = 0;
        int line_advance;
        int pen_y;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
        line_advance = (int)((ascent - descent + line_gap) * scale);
        if (line_advance < 1) {
            line_advance = 1;
        }
        pen_y = (int)(gop->Mode->Info->VerticalResolution / 3) + (int)(ascent * scale);
        for (line = 0; line < 2; line++) {
            int pen_x = (int)(gop->Mode->Info->HorizontalResolution / 4);
            const char *p = lines[line];
            while (*p != '\0') {
                int cp = (unsigned char)(*p);
                int w, h, xoff, yoff;
                int advance_width = 0;
                int left_side_bearing = 0;
                unsigned char *bitmap = stbtt_GetCodepointBitmap(&font, 0, scale, cp, &w, &h, &xoff, &yoff);
                if (bitmap != NULL) {
                    int draw_x = pen_x + xoff;
                    int draw_y = pen_y + yoff;
                    EFI_STATUS draw_status = render_bitmap_with_gop(gop, bitmap, w, h, draw_x, draw_y);
                    if (EFI_ERROR(draw_status)) {
                        Print(L"GOP blit failed for '%c': %r\r\n", cp, draw_status);
                        stbtt_FreeBitmap(bitmap, NULL);
                        FreePool(font_data);
                        return draw_status;
                    }
                    stbtt_FreeBitmap(bitmap, NULL);
                }
                stbtt_GetCodepointHMetrics(&font, cp, &advance_width, &left_side_bearing);
                pen_x += (int)(advance_width * scale);
                (void)left_side_bearing;
                if (*(p + 1) != '\0') {
                    pen_x += (int)(stbtt_GetCodepointKernAdvance(&font, cp, (unsigned char)*(p + 1)) * scale);
                }
                p++;
            }
            pen_y += line_advance;
        }
    }

    Print(L"Rendered strings: ABC / abc\r\n");
    Print(L"Press any key to exit...\r\n");
    wait_for_key(SystemTable);
    FreePool(font_data);
    return status;
}


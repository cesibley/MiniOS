#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define INPUT_MAX 260

typedef enum {
    IMAGE_FMT_UNKNOWN,
    IMAGE_FMT_BMP,
    IMAGE_FMT_JPG,
    IMAGE_FMT_GIF,
    IMAGE_FMT_PNG
} image_format_t;

typedef struct {
    UINT16 Width;
    UINT16 Height;
    union {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bitmap;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *Screen;
    } Image;
} EFI_IMAGE_OUTPUT;

typedef struct _EFI_HII_IMAGE_DECODER_PROTOCOL EFI_HII_IMAGE_DECODER_PROTOCOL;
typedef EFI_STATUS(EFIAPI *EFI_HII_IMAGE_DECODER_DECODE)(
    IN EFI_HII_IMAGE_DECODER_PROTOCOL *This,
    IN VOID *Image,
    IN UINTN ImageRawDataSize,
    IN OUT EFI_IMAGE_OUTPUT **Bitmap,
    IN BOOLEAN Transparent
);

struct _EFI_HII_IMAGE_DECODER_PROTOCOL {
    VOID *GetImageDecoderName;
    VOID *GetImageInfo;
    EFI_HII_IMAGE_DECODER_DECODE DecodeImage;
};

static EFI_GUID gEfiHiiImageDecoderProtocolGuid = {
    0x2f707ebb, 0x4a1a, 0x11d4,
    {0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};

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

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable, CONST CHAR16 *prompt) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    Print(L"\r\n%s", prompt);
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
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

static VOID parse_load_options_path(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable,
                                    CHAR16 *path, UINTN path_max) {
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

static image_format_t detect_format(const UINT8 *data, UINTN size) {
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return IMAGE_FMT_BMP;
    }
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return IMAGE_FMT_JPG;
    }
    if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return IMAGE_FMT_GIF;
    }
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return IMAGE_FMT_PNG;
    }
    return IMAGE_FMT_UNKNOWN;
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

    if (size < 54) {
        return EFI_LOAD_ERROR;
    }

    pixel_offset = rd_u32(data + 10);
    dib_size = rd_u32(data + 14);
    if (dib_size < 40 || 14 + dib_size > size) {
        return EFI_UNSUPPORTED;
    }

    width = rd_s32(data + 18);
    height = rd_s32(data + 22);
    planes = rd_u16(data + 26);
    bpp = rd_u16(data + 28);
    compression = rd_u32(data + 30);

    if (width <= 0 || height == 0 || planes != 1) {
        return EFI_UNSUPPORTED;
    }
    if (compression != 0) {
        return EFI_UNSUPPORTED;
    }
    if (!(bpp == 24 || bpp == 32)) {
        return EFI_UNSUPPORTED;
    }

    abs_h = (height < 0) ? (UINTN)(-height) : (UINTN)height;
    row_stride = (((UINTN)width * (UINTN)bpp + 31U) / 32U) * 4U;
    if (pixel_offset >= size || row_stride == 0) {
        return EFI_LOAD_ERROR;
    }
    if ((UINTN)pixel_offset + row_stride * abs_h > size) {
        return EFI_LOAD_ERROR;
    }

    rgba = AllocatePool((UINTN)width * abs_h * 4U);
    if (rgba == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

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

static EFI_STATUS decode_with_hii_decoder(EFI_SYSTEM_TABLE *SystemTable,
                                          const UINT8 *data, UINTN size,
                                          UINT8 **rgba_out, UINTN *w_out, UINTN *h_out) {
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINTN i;

    status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &gEfiHiiImageDecoderProtocolGuid, NULL,
                               &handle_count, &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < handle_count; i++) {
        EFI_HII_IMAGE_DECODER_PROTOCOL *decoder = NULL;
        EFI_IMAGE_OUTPUT *image = NULL;
        UINTN pixel_count;
        UINT8 *rgba;
        UINTN p;

        status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                   handles[i], &gEfiHiiImageDecoderProtocolGuid, (VOID **)&decoder);
        if (EFI_ERROR(status) || decoder == NULL || decoder->DecodeImage == NULL) {
            continue;
        }

        status = uefi_call_wrapper(decoder->DecodeImage, 5,
                                   decoder, (VOID *)data, size, &image, FALSE);
        if (EFI_ERROR(status) || image == NULL || image->Image.Bitmap == NULL) {
            continue;
        }

        pixel_count = (UINTN)image->Width * (UINTN)image->Height;
        rgba = AllocatePool(pixel_count * 4U);
        if (rgba == NULL) {
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, image);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
            return EFI_OUT_OF_RESOURCES;
        }

        for (p = 0; p < pixel_count; p++) {
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px = &image->Image.Bitmap[p];
            rgba[p * 4U + 0] = px->Red;
            rgba[p * 4U + 1] = px->Green;
            rgba[p * 4U + 2] = px->Blue;
            rgba[p * 4U + 3] = 0xFF;
        }

        *rgba_out = rgba;
        *w_out = image->Width;
        *h_out = image->Height;
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, image);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
        return EFI_SUCCESS;
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
    return EFI_UNSUPPORTED;
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
    if (EFI_ERROR(status)) {
        return status;
    }

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

    if (info->FileSize == 0 || info->FileSize > 64U * 1024U * 1024U) {
        FreePool(info);
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_BAD_BUFFER_SIZE;
    }

    expected_size = (UINTN)info->FileSize;
    data = AllocatePool(expected_size);
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

static EFI_STATUS draw_image_fit(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                 const UINT8 *rgba, UINTN src_w, UINTN src_h) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = {0, 0, 0, 0};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *row;
    UINTN screen_w = gop->Mode->Info->HorizontalResolution;
    UINTN screen_h = gop->Mode->Info->VerticalResolution;
    UINTN draw_w;
    UINTN draw_h;
    UINTN off_x;
    UINTN off_y;
    UINTN x;
    UINTN y;

    if (src_w == 0 || src_h == 0) {
        return EFI_INVALID_PARAMETER;
    }

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

    uefi_call_wrapper(gop->Blt, 10, gop, &black, EfiBltVideoFill,
                      0, 0, 0, 0, screen_w, screen_h, 0);

    row = AllocatePool(sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * draw_w);
    if (row == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    for (y = 0; y < draw_h; y++) {
        UINTN src_y = (y * src_h) / draw_h;
        const UINT8 *src_row = rgba + src_y * src_w * 4U;

        for (x = 0; x < draw_w; x++) {
            UINTN src_x = (x * src_w) / draw_w;
            const UINT8 *p = src_row + src_x * 4U;
            row[x].Red = p[0];
            row[x].Green = p[1];
            row[x].Blue = p[2];
            row[x].Reserved = 0;
        }

        uefi_call_wrapper(gop->Blt, 10, gop, row, EfiBltBufferToVideo,
                          0, 0, off_x, off_y + y, draw_w, 1,
                          sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * draw_w);
    }

    FreePool(row);
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_FILE_HANDLE root = NULL;
    CHAR16 path[INPUT_MAX];
    UINT8 *file_data = NULL;
    UINTN file_size = 0;
    image_format_t format;
    UINT8 *rgba = NULL;
    UINTN w = 0;
    UINTN h = 0;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"IMGVIEW (UEFI)\r\n");
    parse_load_options_path(ImageHandle, SystemTable, path, INPUT_MAX);
    if (path[0] == 0) {
        Print(L"Usage: run IMGVIEW.EFI <image-file>\r\n");
        wait_for_key(SystemTable, L"Press any key to exit...");
        return EFI_INVALID_PARAMETER;
    }

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open filesystem: %r\r\n", status);
        wait_for_key(SystemTable, L"Press any key to exit...");
        return status;
    }

    status = read_file(root, path, &file_data, &file_size);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read '%s': %r\r\n", path, status);
        wait_for_key(SystemTable, L"Press any key to exit...");
        return status;
    }

    format = detect_format(file_data, file_size);
    if (format == IMAGE_FMT_UNKNOWN) {
        Print(L"Unknown or unsupported image format.\r\n");
        FreePool(file_data);
        wait_for_key(SystemTable, L"Press any key to exit...");
        return EFI_UNSUPPORTED;
    }

    if (format == IMAGE_FMT_BMP) {
        status = decode_bmp_rgb(file_data, file_size, &rgba, &w, &h);
    } else {
        if (format == IMAGE_FMT_JPG) Print(L"JPEG detected. Trying firmware image decoders...\r\n");
        if (format == IMAGE_FMT_PNG) Print(L"PNG detected. Trying firmware image decoders...\r\n");
        if (format == IMAGE_FMT_GIF) Print(L"GIF detected. Trying firmware image decoders...\r\n");
        status = decode_with_hii_decoder(SystemTable, file_data, file_size, &rgba, &w, &h);
    }
    FreePool(file_data);
    if (EFI_ERROR(status)) {
        if (format == IMAGE_FMT_BMP) {
            Print(L"Failed to decode BMP: %r\r\n", status);
        } else {
            Print(L"Failed to decode image via firmware decoder: %r\r\n", status);
            Print(L"Tip: if firmware does not expose decoders, convert the file to BMP.\r\n");
        }
        wait_for_key(SystemTable, L"Press any key to exit...");
        return status;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"Graphics Output Protocol not found: %r\r\n", status);
        FreePool(rgba);
        wait_for_key(SystemTable, L"Press any key to exit...");
        return status;
    }

    status = draw_image_fit(gop, rgba, w, h);
    FreePool(rgba);
    if (EFI_ERROR(status)) {
        Print(L"Failed to draw image: %r\r\n", status);
        wait_for_key(SystemTable, L"Press any key to exit...");
        return status;
    }

    wait_for_key(SystemTable, L"Image shown. Press any key to exit...");
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
    return EFI_SUCCESS;
}

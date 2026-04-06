#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define TOKEN_MAX 32

typedef enum {
    CMD_LIST,
    CMD_MODE,
    CMD_SET,
    CMD_HELP
} query_cmd_t;

typedef struct {
    query_cmd_t cmd;
    BOOLEAN has_mode;
    UINT32 mode;
} query_options_t;

static VOID clear_screen(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
}

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    Print(L"\r\nPress any key to exit...");
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
}

static const CHAR16 *pixel_format_name(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
    switch (fmt) {
        case PixelRedGreenBlueReserved8BitPerColor:
            return L"RGBR (8bpc)";
        case PixelBlueGreenRedReserved8BitPerColor:
            return L"BGRR (8bpc)";
        case PixelBitMask:
            return L"BitMask";
        case PixelBltOnly:
            return L"BltOnly";
        default:
            return L"Unknown";
    }
}

static VOID print_mode_details(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, UINT32 mode) {
    Print(L"mode %u\r\n", mode);
    Print(L"  resolution   : %ux%u\r\n", info->HorizontalResolution, info->VerticalResolution);
    Print(L"  pixel format : %s\r\n", pixel_format_name(info->PixelFormat));
    Print(L"  pixels/scan  : %u\r\n", info->PixelsPerScanLine);

    if (info->PixelFormat == PixelBitMask) {
        Print(L"  bitmasks     : R=%08x G=%08x B=%08x A=%08x\r\n",
              info->PixelInformation.RedMask,
              info->PixelInformation.GreenMask,
              info->PixelInformation.BlueMask,
              info->PixelInformation.ReservedMask);
    }
}

static BOOLEAN token_equals(CHAR16 *a, const CHAR16 *b) {
    UINTN i = 0;

    while (a[i] != 0 && b[i] != 0) {
        CHAR16 ca = a[i];
        CHAR16 cb = b[i];

        if (ca >= L'A' && ca <= L'Z') {
            ca = (CHAR16)(ca - L'A' + L'a');
        }
        if (cb >= L'A' && cb <= L'Z') {
            cb = (CHAR16)(cb - L'A' + L'a');
        }
        if (ca != cb) {
            return FALSE;
        }
        i++;
    }

    return a[i] == 0 && b[i] == 0;
}

static BOOLEAN parse_uint32_token(CHAR16 *token, UINT32 *value) {
    UINTN i = 0;
    UINT64 v = 0;

    if (token[0] == 0) {
        return FALSE;
    }

    while (token[i] != 0) {
        if (token[i] < L'0' || token[i] > L'9') {
            return FALSE;
        }
        v = (v * 10) + (UINT64)(token[i] - L'0');
        if (v > 0xFFFFFFFFULL) {
            return FALSE;
        }
        i++;
    }

    *value = (UINT32)v;
    return TRUE;
}

static UINTN tokenize_load_options(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable,
                                   CHAR16 tokens[3][TOKEN_MAX]) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status;
    CHAR16 *options;
    UINTN options_len;
    UINTN i = 0;
    UINTN t = 0;

    for (t = 0; t < 3; t++) {
        tokens[t][0] = 0;
    }
    t = 0;

    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                               ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL || loaded_image->LoadOptions == NULL ||
        loaded_image->LoadOptionsSize == 0) {
        return 0;
    }

    options = (CHAR16 *)loaded_image->LoadOptions;
    options_len = loaded_image->LoadOptionsSize / sizeof(CHAR16);

    while (i < options_len && options[i] != 0 && t < 3) {
        UINTN out = 0;

        while (i < options_len && (options[i] == L' ' || options[i] == L'\t')) {
            i++;
        }
        if (i >= options_len || options[i] == 0) {
            break;
        }

        if (options[i] == L'"') {
            i++;
            while (i < options_len && options[i] != 0 && options[i] != L'"' && out + 1 < TOKEN_MAX) {
                tokens[t][out++] = options[i++];
            }
            if (i < options_len && options[i] == L'"') {
                i++;
            }
        } else {
            while (i < options_len && options[i] != 0 && options[i] != L' ' && options[i] != L'\t' &&
                   out + 1 < TOKEN_MAX) {
                tokens[t][out++] = options[i++];
            }
        }

        tokens[t][out] = 0;
        t++;
    }

    return t;
}

static query_options_t parse_options(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 tokens[3][TOKEN_MAX];
    UINTN count;
    query_options_t options;

    options.cmd = CMD_LIST;
    options.has_mode = FALSE;
    options.mode = 0;

    count = tokenize_load_options(ImageHandle, SystemTable, tokens);
    if (count == 0) {
        return options;
    }

    if (token_equals(tokens[0], L"help") || token_equals(tokens[0], L"-h") || token_equals(tokens[0], L"--help")) {
        options.cmd = CMD_HELP;
        return options;
    }

    if (token_equals(tokens[0], L"mode") && count >= 2 && parse_uint32_token(tokens[1], &options.mode)) {
        options.cmd = CMD_MODE;
        options.has_mode = TRUE;
        return options;
    }

    if (token_equals(tokens[0], L"set") && count >= 2 && parse_uint32_token(tokens[1], &options.mode)) {
        options.cmd = CMD_SET;
        options.has_mode = TRUE;
        return options;
    }

    if (count >= 1 && parse_uint32_token(tokens[0], &options.mode)) {
        options.cmd = CMD_MODE;
        options.has_mode = TRUE;
        return options;
    }

    options.cmd = CMD_HELP;
    return options;
}

static VOID print_usage(VOID) {
    Print(L"Usage:\r\n");
    Print(L"  run GOPQUERY.EFI              # list all GOP modes\r\n");
    Print(L"  run GOPQUERY.EFI mode <N>     # query one mode\r\n");
    Print(L"  run GOPQUERY.EFI <N>          # shorthand for mode <N>\r\n");
    Print(L"  run GOPQUERY.EFI set <N>      # switch GOP mode then show current info\r\n");
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    query_options_t options;
    UINT32 mode;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    clear_screen(SystemTable);

    Print(L"GOP Query (UEFI Graphics Output Protocol)\r\n\r\n");

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"Graphics Output Protocol not found: %r\r\n", status);
        wait_for_key(SystemTable);
        clear_screen(SystemTable);
        return status;
    }

    options = parse_options(ImageHandle, SystemTable);

    Print(L"Current mode      : %u\r\n", gop->Mode->Mode);
    Print(L"Available modes   : %u\r\n", gop->Mode->MaxMode);
    Print(L"Framebuffer base  : 0x%lx\r\n", (UINT64)gop->Mode->FrameBufferBase);
    Print(L"Framebuffer size  : %lu bytes\r\n\r\n", (UINT64)gop->Mode->FrameBufferSize);

    if (options.cmd == CMD_HELP) {
        print_usage();
        wait_for_key(SystemTable);
        clear_screen(SystemTable);
        return EFI_INVALID_PARAMETER;
    }

    if (options.cmd == CMD_SET) {
        if (!options.has_mode || options.mode >= gop->Mode->MaxMode) {
            Print(L"Invalid mode index %u (max %u).\r\n\r\n", options.mode,
                  gop->Mode->MaxMode > 0 ? gop->Mode->MaxMode - 1 : 0);
            print_usage();
            wait_for_key(SystemTable);
            clear_screen(SystemTable);
            return EFI_INVALID_PARAMETER;
        }

        status = uefi_call_wrapper(gop->SetMode, 2, gop, options.mode);
        if (EFI_ERROR(status)) {
            Print(L"SetMode(%u) failed: %r\r\n", options.mode, status);
            wait_for_key(SystemTable);
            clear_screen(SystemTable);
            return status;
        }

        Print(L"Set GOP mode to %u successfully.\r\n\r\n", options.mode);
    }

    if (options.cmd == CMD_MODE || options.cmd == CMD_SET) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;

        mode = (options.cmd == CMD_SET) ? gop->Mode->Mode : options.mode;
        if (mode >= gop->Mode->MaxMode) {
            Print(L"Invalid mode index %u (max %u).\r\n", mode,
                  gop->Mode->MaxMode > 0 ? gop->Mode->MaxMode - 1 : 0);
            wait_for_key(SystemTable);
            clear_screen(SystemTable);
            return EFI_INVALID_PARAMETER;
        }

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status)) {
            Print(L"QueryMode(%u) failed: %r\r\n", mode, status);
            wait_for_key(SystemTable);
            clear_screen(SystemTable);
            return status;
        }

        print_mode_details(info, mode);
        wait_for_key(SystemTable);
        clear_screen(SystemTable);
        return EFI_SUCCESS;
    }

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status)) {
            Print(L"mode %u: query failed (%r)\r\n", mode, status);
            continue;
        }

        Print(L"mode %u: %ux%u, format=%s, ppsl=%u\r\n",
              mode,
              info->HorizontalResolution,
              info->VerticalResolution,
              pixel_format_name(info->PixelFormat),
              info->PixelsPerScanLine);
    }

    wait_for_key(SystemTable);
    clear_screen(SystemTable);
    return EFI_SUCCESS;
}

#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

static VOID cls(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
}

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable, CONST CHAR16 *prompt) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    Print(L"\r\n%s", prompt);
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
}

static UINTN count_set_bits(UINT32 value) {
    UINTN bits = 0;
    while (value != 0) {
        bits += (UINTN)(value & 1U);
        value >>= 1;
    }
    return bits;
}

static CONST CHAR16 *pixel_format_name(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
    switch (fmt) {
    case PixelRedGreenBlueReserved8BitPerColor:
        return L"RGBR 8:8:8:8";
    case PixelBlueGreenRedReserved8BitPerColor:
        return L"BGRR 8:8:8:8";
    case PixelBitMask:
        return L"BitMask";
    case PixelBltOnly:
        return L"BltOnly";
    default:
        return L"Unknown";
    }
}

static UINTN mode_bit_depth(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
        info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        return 32;
    }

    if (info->PixelFormat == PixelBitMask) {
        return count_set_bits(info->PixelInformation.RedMask) +
               count_set_bits(info->PixelInformation.GreenMask) +
               count_set_bits(info->PixelInformation.BlueMask) +
               count_set_bits(info->PixelInformation.ReservedMask);
    }

    return 0;
}

static VOID fill_screen_with_color_bars(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL colors[8] = {
        {0x00, 0x00, 0x00, 0x00}, /* Black */
        {0x00, 0x00, 0xFF, 0x00}, /* Red */
        {0x00, 0xFF, 0x00, 0x00}, /* Green */
        {0xFF, 0x00, 0x00, 0x00}, /* Blue */
        {0x00, 0xFF, 0xFF, 0x00}, /* Yellow */
        {0xFF, 0xFF, 0x00, 0x00}, /* Cyan */
        {0xFF, 0x00, 0xFF, 0x00}, /* Magenta */
        {0xFF, 0xFF, 0xFF, 0x00}  /* White */
    };
    UINTN width = gop->Mode->Info->HorizontalResolution;
    UINTN height = gop->Mode->Info->VerticalResolution;
    UINTN bar_width = width / 8;
    UINTN i;

    for (i = 0; i < 8; i++) {
        UINTN x = i * bar_width;
        UINTN draw_width = (i == 7) ? (width - x) : bar_width;
        uefi_call_wrapper(gop->Blt, 10,
                          gop,
                          &colors[i],
                          EfiBltVideoFill,
                          0, 0,
                          x, 0,
                          draw_width, height,
                          0);
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    UINT32 mode;
    UINT32 original_mode;
    (void)ImageHandle;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    cls(SystemTable);

    Print(L"GFX Test (UEFI GOP)\r\n");

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"Graphics Output Protocol not found: %r\r\n", status);
        wait_for_key(SystemTable, L"Press any key to exit...");
        cls(SystemTable);
        return status;
    }

    Print(L"Detected %d GOP mode(s)\r\n", gop->Mode->MaxMode);
    Print(L"Current mode: %d\r\n", gop->Mode->Mode);
    original_mode = gop->Mode->Mode;

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;
        UINTN bpp;
        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status)) {
            Print(L"  mode %d: query failed (%r)\r\n", mode, status);
            continue;
        }

        status = uefi_call_wrapper(gop->SetMode, 2, gop, mode);
        if (EFI_ERROR(status)) {
            Print(L"  SetMode failed: %r\r\n", status);
            FreePool(info);
            continue;
        }

        bpp = mode_bit_depth(info);
        fill_screen_with_color_bars(gop);
        uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
        Print(L"Mode %d of %d\r\n", mode + 1, gop->Mode->MaxMode);
        Print(L"Resolution: %dx%d\r\n", info->HorizontalResolution, info->VerticalResolution);
        if (bpp != 0) {
            Print(L"Bit depth: %d bpp\r\n", bpp);
        } else {
            Print(L"Bit depth: unknown\r\n");
        }
        Print(L"Pixel format: %s\r\n", pixel_format_name(info->PixelFormat));
        Print(L"Pixels/scan line: %d\r\n", info->PixelsPerScanLine);
        if (info->PixelFormat == PixelBitMask) {
            Print(L"Masks R/G/B/Res: %08x/%08x/%08x/%08x\r\n",
                  info->PixelInformation.RedMask,
                  info->PixelInformation.GreenMask,
                  info->PixelInformation.BlueMask,
                  info->PixelInformation.ReservedMask);
        }
        wait_for_key(SystemTable, L"Press any key for next mode...");
        FreePool(info);
    }

    status = uefi_call_wrapper(gop->SetMode, 2, gop, original_mode);
    if (EFI_ERROR(status)) {
        Print(L"Failed to restore original mode %d: %r\r\n", original_mode, status);
    } else {
        Print(L"Restored original mode %d.\r\n", original_mode);
    }

    Print(L"\r\nAll modes shown.\r\n");
    wait_for_key(SystemTable, L"Press any key to exit...");
    cls(SystemTable);
    return EFI_SUCCESS;
}

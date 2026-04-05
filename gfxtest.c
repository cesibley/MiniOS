#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

static VOID cls(EFI_SYSTEM_TABLE *SystemTable) {
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
    (void)ImageHandle;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);
    cls(SystemTable);

    Print(L"GFX Test (UEFI GOP)\r\n");

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"Graphics Output Protocol not found: %r\r\n", status);
        wait_for_key(SystemTable);
        cls(SystemTable);
        return status;
    }

    Print(L"Detected %d GOP mode(s)\r\n", gop->Mode->MaxMode);
    Print(L"Current mode: %d\r\n", gop->Mode->Mode);

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;
        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status)) {
            Print(L"  mode %d: query failed (%r)\r\n", mode, status);
            continue;
        }

        Print(L"  mode %d: %dx%d\r\n", mode, info->HorizontalResolution, info->VerticalResolution);
    }

    Print(L"\r\nDrawing color bars on current mode (%dx%d)...\r\n",
          gop->Mode->Info->HorizontalResolution,
          gop->Mode->Info->VerticalResolution);
    fill_screen_with_color_bars(gop);

    wait_for_key(SystemTable);
    cls(SystemTable);
    return EFI_SUCCESS;
}

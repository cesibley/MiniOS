#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef EFI_SIMPLE_POINTER_PROTOCOL_GUID
#define EFI_SIMPLE_POINTER_PROTOCOL_GUID \
    { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#endif

static VOID fill_rect(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                      UINTN x,
                      UINTN y,
                      UINTN width,
                      UINTN height,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    uefi_call_wrapper(gop->Blt, 10,
                      gop,
                      &color,
                      EfiBltVideoFill,
                      0, 0,
                      x, y,
                      width, height,
                      0);
}

static VOID draw_brush(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                       UINTN x,
                       UINTN y,
                       EFI_GRAPHICS_OUTPUT_BLT_PIXEL color,
                       UINTN radius) {
    INTN dx;
    INTN dy;

    for (dy = -(INTN)radius; dy <= (INTN)radius; dy++) {
        for (dx = -(INTN)radius; dx <= (INTN)radius; dx++) {
            UINTN px;
            UINTN py;
            INTN dist2 = dx * dx + dy * dy;
            if (dist2 > (INTN)(radius * radius)) {
                continue;
            }

            if ((INTN)x + dx < 0 || (INTN)y + dy < 0) {
                continue;
            }

            px = (UINTN)((INTN)x + dx);
            py = (UINTN)((INTN)y + dy);
            if (px >= gop->Mode->Info->HorizontalResolution ||
                py >= gop->Mode->Info->VerticalResolution) {
                continue;
            }

            fill_rect(gop, px, py, 1, 1, color);
        }
    }
}

static EFI_STATUS set_highest_resolution_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    EFI_STATUS status;
    UINT32 mode;
    UINT32 best_mode = gop->Mode->Mode;
    UINTN best_area = 0;

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;
        UINTN area;

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status)) {
            continue;
        }

        area = (UINTN)info->HorizontalResolution * (UINTN)info->VerticalResolution;
        if (area > best_area) {
            best_area = area;
            best_mode = mode;
        }
        FreePool(info);
    }

    return uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_SIMPLE_POINTER_PROTOCOL *pointer = NULL;
    EFI_GUID simple_pointer_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_EVENT events[2];
    UINTN event_index;
    UINTN width;
    UINTN height;
    UINTN cursor_x;
    UINTN cursor_y;
    UINTN color_index = 1;
    UINTN i;
    UINTN palette_height = 24;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL colors[] = {
        {0xFF, 0xFF, 0xFF, 0x00}, /* white */
        {0x00, 0x00, 0xFF, 0x00}, /* red */
        {0x00, 0xFF, 0x00, 0x00}, /* green */
        {0xFF, 0x00, 0x00, 0x00}, /* blue */
        {0x00, 0xFF, 0xFF, 0x00}, /* yellow */
        {0xFF, 0xFF, 0x00, 0x00}, /* cyan */
        {0xFF, 0x00, 0xFF, 0x00}, /* magenta */
        {0x00, 0x00, 0x00, 0x00}  /* black */
    };
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg = {0x30, 0x30, 0x30, 0x00};

    (void)ImageHandle;
    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"Paint demo requires GOP: %r\r\n", status);
        return status;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &simple_pointer_guid, NULL, (VOID **)&pointer);
    if (EFI_ERROR(status) || pointer == NULL) {
        Print(L"Paint demo requires a mouse/simple pointer: %r\r\n", status);
        return status;
    }

    status = set_highest_resolution_mode(gop);
    if (EFI_ERROR(status)) {
        Print(L"Unable to switch graphics mode: %r\r\n", status);
        return status;
    }

    width = gop->Mode->Info->HorizontalResolution;
    height = gop->Mode->Info->VerticalResolution;
    cursor_x = width / 2;
    cursor_y = height / 2;

    fill_rect(gop, 0, 0, width, height, bg);
    for (i = 0; i < ARRAY_SIZE(colors); i++) {
        UINTN box_w = width / ARRAY_SIZE(colors);
        UINTN x = i * box_w;
        UINTN w = (i == ARRAY_SIZE(colors) - 1) ? (width - x) : box_w;
        fill_rect(gop, x, 0, w, palette_height, colors[i]);
    }

    Print(L"MiniOS UEFI Paint demo\r\n");
    Print(L"Mouse move + left button: paint, right button: clear\r\n");
    Print(L"Click palette on top row to change color.\r\n");
    Print(L"Keyboard: [C] next color, [Q] or [Esc] quit\r\n");

    status = uefi_call_wrapper(pointer->Reset, 2, pointer, FALSE);
    if (EFI_ERROR(status)) {
        Print(L"Pointer reset warning: %r\r\n", status);
    }

    events[0] = pointer->WaitForInput;
    events[1] = SystemTable->ConIn->WaitForKey;

    for (;;) {
        status = uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                                   2, events, &event_index);
        if (EFI_ERROR(status)) {
            break;
        }

        if (event_index == 0) {
            EFI_SIMPLE_POINTER_STATE state;

            while (!EFI_ERROR(uefi_call_wrapper(pointer->GetState, 2, pointer, &state))) {
                if (state.RelativeMovementX != 0) {
                    INTN nx = (INTN)cursor_x + (state.RelativeMovementX / 4);
                    if (nx < 0) {
                        nx = 0;
                    }
                    if ((UINTN)nx >= width) {
                        nx = (INTN)width - 1;
                    }
                    cursor_x = (UINTN)nx;
                }

                if (state.RelativeMovementY != 0) {
                    INTN ny = (INTN)cursor_y + (state.RelativeMovementY / 4);
                    if (ny < (INTN)palette_height) {
                        ny = (INTN)palette_height;
                    }
                    if ((UINTN)ny >= height) {
                        ny = (INTN)height - 1;
                    }
                    cursor_y = (UINTN)ny;
                }

                if (state.LeftButton) {
                    UINTN box_w = width / ARRAY_SIZE(colors);
                    if (cursor_y < palette_height) {
                        UINTN selected = cursor_x / box_w;
                        if (selected >= ARRAY_SIZE(colors)) {
                            selected = ARRAY_SIZE(colors) - 1;
                        }
                        color_index = selected;
                    } else {
                        draw_brush(gop, cursor_x, cursor_y, colors[color_index], 4);
                    }
                }

                if (state.RightButton) {
                    fill_rect(gop, 0, palette_height, width, height - palette_height, bg);
                }

                draw_brush(gop, cursor_x, cursor_y, colors[color_index], 1);
            }
        } else {
            EFI_INPUT_KEY key;
            if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                                             SystemTable->ConIn, &key))) {
                if (key.UnicodeChar == L'q' || key.UnicodeChar == L'Q' || key.ScanCode == SCAN_ESC) {
                    break;
                }
                if (key.UnicodeChar == L'c' || key.UnicodeChar == L'C') {
                    color_index = (color_index + 1) % ARRAY_SIZE(colors);
                }
            }
        }
    }

    return EFI_SUCCESS;
}

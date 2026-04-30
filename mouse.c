#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

static VOID connect_all_controllers(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINTN i;

    status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
                               AllHandles, NULL, NULL, &handle_count, &handles);
    if (EFI_ERROR(status)) {
        Print(L"[debug] LocateHandleBuffer(AllHandles) failed: %r\r\n", status);
        return;
    }

    for (i = 0; i < handle_count; i++) {
        (VOID)uefi_call_wrapper(SystemTable->BootServices->ConnectController, 4,
                                handles[i], NULL, NULL, TRUE);
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
}

static UINTN clamp_u(UINTN value, UINTN max_value) {
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static VOID fill_rect(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                      UINTN x,
                      UINTN y,
                      UINTN w,
                      UINTN h,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    if (w == 0 || h == 0) {
        return;
    }

    (VOID)uefi_call_wrapper(gop->Blt, 10,
                            gop,
                            &color,
                            EfiBltVideoFill,
                            0,
                            0,
                            x,
                            y,
                            w,
                            h,
                            0);
}

static VOID draw_pointer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                         UINTN x,
                         UINTN y,
                         EFI_GRAPHICS_OUTPUT_BLT_PIXEL line_color,
                         EFI_GRAPHICS_OUTPUT_BLT_PIXEL center_color) {
    UINTN max_x = gop->Mode->Info->HorizontalResolution - 1;
    UINTN max_y = gop->Mode->Info->VerticalResolution - 1;
    UINTN left = (x > 8) ? (x - 8) : 0;
    UINTN top = (y > 8) ? (y - 8) : 0;
    UINTN right = (x + 8 > max_x) ? max_x : (x + 8);
    UINTN bottom = (y + 8 > max_y) ? max_y : (y + 8);

    fill_rect(gop, left, y, (right - left) + 1, 1, line_color);
    fill_rect(gop, x, top, 1, (bottom - top) + 1, line_color);

    if (x > 0 && y > 0 && x < max_x && y < max_y) {
        fill_rect(gop, x - 1, y - 1, 3, 3, center_color);
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    typedef enum {
        POINTER_MODE_AUTO = 0,
        POINTER_MODE_ABSOLUTE,
        POINTER_MODE_SIMPLE
    } pointer_mode_t;

    EFI_STATUS status;
    EFI_GUID gop_guid = gEfiGraphicsOutputProtocolGuid;
    EFI_GUID simple_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_GUID absolute_guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_SIMPLE_POINTER_PROTOCOL *simple_pointer = NULL;
    EFI_ABSOLUTE_POINTER_PROTOCOL *absolute_pointer = NULL;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    EFI_INPUT_KEY key;
    EFI_EVENT events[3];
    UINTN event_count = 0;
    UINTN event_index;
    UINTN pointer_x;
    UINTN pointer_y;
    UINT64 last_abs_x = 0;
    UINT64 last_abs_y = 0;
    UINT32 last_abs_buttons = 0;
    pointer_mode_t pointer_mode = POINTER_MODE_AUTO;
    UINTN abs_stale_events = 0;

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg = { .Blue = 10, .Green = 10, .Red = 10, .Reserved = 0 };
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL fg = { .Blue = 255, .Green = 255, .Red = 255, .Reserved = 0 };
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL center = { .Blue = 64, .Green = 64, .Red = 255, .Reserved = 0 };

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);

    Print(L"Mouse Test (GOP graphical mode)\r\n");
    Print(L"[debug] Initializing...\r\n");

    connect_all_controllers(SystemTable);

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gop_guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        Print(L"[debug] GOP unavailable: %r\r\n", status);
        return EFI_UNSUPPORTED;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &simple_guid, NULL, &handle_count, &handles);
    if (!EFI_ERROR(status) && handle_count > 0) {
        (VOID)uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                handles[0], &simple_guid, (VOID **)&simple_pointer);
    }
    if (handles != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
        handles = NULL;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &absolute_guid, NULL, &handle_count, &handles);
    if (!EFI_ERROR(status) && handle_count > 0) {
        (VOID)uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
                                handles[0], &absolute_guid, (VOID **)&absolute_pointer);
    }
    if (handles != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, handles);
        handles = NULL;
    }

    if (simple_pointer == NULL && absolute_pointer == NULL) {
        Print(L"[debug] No pointer protocol found.\r\n");
        return EFI_UNSUPPORTED;
    }

    if (simple_pointer != NULL) {
        (VOID)uefi_call_wrapper(simple_pointer->Reset, 2, simple_pointer, TRUE);
    }
    if (absolute_pointer != NULL) {
        (VOID)uefi_call_wrapper(absolute_pointer->Reset, 2, absolute_pointer, TRUE);
    }

    fill_rect(gop,
              0,
              0,
              gop->Mode->Info->HorizontalResolution,
              gop->Mode->Info->VerticalResolution,
              bg);

    pointer_x = gop->Mode->Info->HorizontalResolution / 2;
    pointer_y = gop->Mode->Info->VerticalResolution / 2;
    draw_pointer(gop, pointer_x, pointer_y, fg, center);

    events[event_count++] = SystemTable->ConIn->WaitForKey;
    if (simple_pointer != NULL && simple_pointer->WaitForInput != NULL) {
        events[event_count++] = simple_pointer->WaitForInput;
    }
    if (absolute_pointer != NULL && absolute_pointer->WaitForInput != NULL) {
        events[event_count++] = absolute_pointer->WaitForInput;
    }

    if (event_count == 1) {
        Print(L"[debug] Pointer protocol found but no WaitForInput event.\r\n");
        return EFI_UNSUPPORTED;
    }

    while (1) {
        status = uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                                   event_count, events, &event_index);
        if (EFI_ERROR(status)) {
            break;
        }

        if (event_index == 0) {
            if (!EFI_ERROR(uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                                             SystemTable->ConIn, &key))) {
                break;
            }
            continue;
        }

        draw_pointer(gop, pointer_x, pointer_y, bg, bg);

        {
            BOOLEAN used_absolute = FALSE;
            BOOLEAN used_simple = FALSE;

            if (absolute_pointer != NULL && pointer_mode != POINTER_MODE_SIMPLE) {
            EFI_ABSOLUTE_POINTER_STATE state;
            BOOLEAN abs_changed = FALSE;
            status = uefi_call_wrapper(absolute_pointer->GetState, 2, absolute_pointer, &state);
            if (!EFI_ERROR(status)) {
                if (state.CurrentX != last_abs_x || state.CurrentY != last_abs_y ||
                    state.ActiveButtons != last_abs_buttons) {
                    abs_changed = TRUE;
                }
                UINT64 min_x = absolute_pointer->Mode->AbsoluteMinX;
                UINT64 max_x = absolute_pointer->Mode->AbsoluteMaxX;
                UINT64 min_y = absolute_pointer->Mode->AbsoluteMinY;
                UINT64 max_y = absolute_pointer->Mode->AbsoluteMaxY;
                UINT64 range_x = (max_x > min_x) ? (max_x - min_x) : 1;
                UINT64 range_y = (max_y > min_y) ? (max_y - min_y) : 1;
                UINT64 off_x = (state.CurrentX > min_x) ? (state.CurrentX - min_x) : 0;
                UINT64 off_y = (state.CurrentY > min_y) ? (state.CurrentY - min_y) : 0;

                if (abs_changed) {
                    pointer_x = (UINTN)((off_x * (gop->Mode->Info->HorizontalResolution - 1)) / range_x);
                    pointer_y = (UINTN)((off_y * (gop->Mode->Info->VerticalResolution - 1)) / range_y);
                    used_absolute = TRUE;
                    abs_stale_events = 0;
                } else {
                    abs_stale_events++;
                }

                last_abs_x = state.CurrentX;
                last_abs_y = state.CurrentY;
                last_abs_buttons = state.ActiveButtons;
            }
            }

            if (simple_pointer != NULL &&
                ((pointer_mode == POINTER_MODE_SIMPLE) ||
                 (pointer_mode == POINTER_MODE_AUTO && !used_absolute) ||
                 (pointer_mode == POINTER_MODE_ABSOLUTE && abs_stale_events > 30))) {
            EFI_SIMPLE_POINTER_STATE state;
            status = uefi_call_wrapper(simple_pointer->GetState, 2, simple_pointer, &state);
            if (!EFI_ERROR(status)) {
                INTN scale_x = 1024;
                INTN scale_y = 1024;
                INTN step_x;
                INTN step_y;

                if (simple_pointer->Mode != NULL) {
                    if (simple_pointer->Mode->ResolutionX > 0) {
                        scale_x = (INTN)(simple_pointer->Mode->ResolutionX / 64);
                    }
                    if (simple_pointer->Mode->ResolutionY > 0) {
                        scale_y = (INTN)(simple_pointer->Mode->ResolutionY / 64);
                    }
                }
                if (scale_x < 1) {
                    scale_x = 1;
                }
                if (scale_y < 1) {
                    scale_y = 1;
                }

                step_x = (INTN)(state.RelativeMovementX / scale_x);
                step_y = (INTN)(state.RelativeMovementY / scale_y);

                if (step_x == 0 && state.RelativeMovementX != 0) {
                    step_x = (state.RelativeMovementX > 0) ? 1 : -1;
                }
                if (step_y == 0 && state.RelativeMovementY != 0) {
                    step_y = (state.RelativeMovementY > 0) ? 1 : -1;
                }

                if (step_x != 0 || step_y != 0) {
                    INTN next_x = (INTN)pointer_x + step_x;
                    INTN next_y = (INTN)pointer_y + step_y;
                    if (next_x < 0) {
                        next_x = 0;
                    }
                    if (next_y < 0) {
                        next_y = 0;
                    }
                    pointer_x = clamp_u((UINTN)next_x, gop->Mode->Info->HorizontalResolution - 1);
                    pointer_y = clamp_u((UINTN)next_y, gop->Mode->Info->VerticalResolution - 1);
                    used_simple = TRUE;
                }
            }
        }

            if (pointer_mode == POINTER_MODE_AUTO) {
                if (used_absolute) {
                    pointer_mode = POINTER_MODE_ABSOLUTE;
                } else if (used_simple) {
                    pointer_mode = POINTER_MODE_SIMPLE;
                }
            } else if (pointer_mode == POINTER_MODE_ABSOLUTE && used_simple) {
                pointer_mode = POINTER_MODE_SIMPLE;
            }
        }

        draw_pointer(gop, pointer_x, pointer_y, fg, center);
    }

    return EFI_SUCCESS;
}

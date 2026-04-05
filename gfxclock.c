#include <efi.h>
#include <efilib.h>
#include "watchdog.h"

#define UNIT_SCALE 1024

static const INTN UNIT_X[60] = {
    0, 107, 213, 316, 416, 512, 602, 685, 761, 828,
    887, 935, 974, 1002, 1018, 1024, 1018, 1002, 974, 935,
    887, 828, 761, 685, 602, 512, 416, 316, 213, 107,
    0, -107, -213, -316, -416, -512, -602, -685, -761, -828,
    -887, -935, -974, -1002, -1018, -1024, -1018, -1002, -974, -935,
    -887, -828, -761, -685, -602, -512, -416, -316, -213, -107
};

static const INTN UNIT_Y[60] = {
    -1024, -1018, -1002, -974, -935, -887, -828, -761, -685, -602,
    -512, -416, -316, -213, -107, 0, 107, 213, 316, 416,
    512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018,
    1024, 1018, 1002, 974, 935, 887, 828, 761, 685, 602,
    512, 416, 316, 213, 107, 0, -107, -213, -316, -416,
    -512, -602, -685, -761, -828, -887, -935, -974, -1002, -1018
};

static VOID fill_rect(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                      UINTN x,
                      UINTN y,
                      UINTN width,
                      UINTN height,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    if (width == 0 || height == 0) {
        return;
    }

    uefi_call_wrapper(gop->Blt, 10,
                      gop,
                      &color,
                      EfiBltVideoFill,
                      0, 0,
                      x, y,
                      width, height,
                      0);
}

static VOID plot(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                 INTN x,
                 INTN y,
                 EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    UINTN width = gop->Mode->Info->HorizontalResolution;
    UINTN height = gop->Mode->Info->VerticalResolution;

    if (x < 0 || y < 0 || (UINTN)x >= width || (UINTN)y >= height) {
        return;
    }

    fill_rect(gop, (UINTN)x, (UINTN)y, 1, 1, color);
}

static INTN iabs(INTN v) {
    return (v < 0) ? -v : v;
}

static VOID draw_line(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                      INTN x0,
                      INTN y0,
                      INTN x1,
                      INTN y1,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    INTN dx = iabs(x1 - x0);
    INTN sx = (x0 < x1) ? 1 : -1;
    INTN dy = -iabs(y1 - y0);
    INTN sy = (y0 < y1) ? 1 : -1;
    INTN err = dx + dy;

    while (1) {
        INTN e2;

        plot(gop, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }

        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static VOID draw_circle(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                        INTN cx,
                        INTN cy,
                        INTN radius,
                        EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    INTN x = radius;
    INTN y = 0;
    INTN err = 1 - x;

    while (x >= y) {
        plot(gop, cx + x, cy + y, color);
        plot(gop, cx + y, cy + x, color);
        plot(gop, cx - y, cy + x, color);
        plot(gop, cx - x, cy + y, color);
        plot(gop, cx - x, cy - y, color);
        plot(gop, cx - y, cy - x, color);
        plot(gop, cx + y, cy - x, color);
        plot(gop, cx + x, cy - y, color);

        y++;
        if (err < 0) {
            err += (2 * y) + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static VOID draw_ticks(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                       INTN cx,
                       INTN cy,
                       INTN radius,
                       EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    UINTN i;

    for (i = 0; i < 60; i++) {
        INTN outer_x = cx + (UNIT_X[i] * radius) / UNIT_SCALE;
        INTN outer_y = cy + (UNIT_Y[i] * radius) / UNIT_SCALE;
        INTN inner_radius = radius - ((i % 5 == 0) ? (radius / 8) : (radius / 16));
        INTN inner_x = cx + (UNIT_X[i] * inner_radius) / UNIT_SCALE;
        INTN inner_y = cy + (UNIT_Y[i] * inner_radius) / UNIT_SCALE;
        draw_line(gop, inner_x, inner_y, outer_x, outer_y, color);
    }
}

static VOID draw_hand(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                      INTN cx,
                      INTN cy,
                      UINTN tick,
                      INTN length,
                      INTN thickness,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
    INTN end_x = cx + (UNIT_X[tick % 60] * length) / UNIT_SCALE;
    INTN end_y = cy + (UNIT_Y[tick % 60] * length) / UNIT_SCALE;
    INTN t;

    for (t = -thickness; t <= thickness; t++) {
        draw_line(gop, cx + t, cy, end_x + t, end_y, color);
        draw_line(gop, cx, cy + t, end_x, end_y + t, color);
    }
}

static EFI_STATUS locate_gop(EFI_SYSTEM_TABLE *SystemTable,
                             EFI_GRAPHICS_OUTPUT_PROTOCOL **gop_out) {
    return uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                             &gEfiGraphicsOutputProtocolGuid,
                             NULL,
                             (VOID **)gop_out);
}

static BOOLEAN key_pressed(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    EFI_STATUS status = uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                                          SystemTable->ConIn,
                                          &key);
    return !EFI_ERROR(status);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg = {0x18, 0x18, 0x18, 0x00};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL dial = {0xE0, 0xE0, 0xE0, 0x00};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL hour_color = {0xD0, 0xD0, 0x40, 0x00};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL minute_color = {0x80, 0xF0, 0x80, 0x00};
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL second_color = {0x40, 0x60, 0xFF, 0x00};
    EFI_TIME now;
    INTN last_second = -1;
    UINTN width;
    UINTN height;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);

    status = locate_gop(SystemTable, &gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"GOP unavailable: %r\r\n", status);
        return status;
    }

    width = gop->Mode->Info->HorizontalResolution;
    height = gop->Mode->Info->VerticalResolution;

    while (1) {
        INTN cx;
        INTN cy;
        INTN radius;
        UINTN second_tick;
        UINTN minute_tick;
        UINTN hour_tick;

        status = uefi_call_wrapper(SystemTable->RuntimeServices->GetTime, 2, &now, NULL);
        if (EFI_ERROR(status)) {
            Print(L"GetTime failed: %r\r\n", status);
            return status;
        }

        if ((INTN)now.Second != last_second) {
            last_second = now.Second;

            cx = (INTN)width / 2;
            cy = (INTN)height / 2;
            radius = (INTN)(((width < height) ? width : height) * 40 / 100);
            if (radius < 30) {
                radius = 30;
            }

            fill_rect(gop, 0, 0, width, height, bg);
            draw_circle(gop, cx, cy, radius, dial);
            draw_ticks(gop, cx, cy, radius, dial);

            second_tick = now.Second % 60;
            minute_tick = now.Minute % 60;
            hour_tick = ((now.Hour % 12) * 5 + (now.Minute / 12)) % 60;

            draw_hand(gop, cx, cy, hour_tick, (radius * 50) / 100, 2, hour_color);
            draw_hand(gop, cx, cy, minute_tick, (radius * 75) / 100, 1, minute_color);
            draw_hand(gop, cx, cy, second_tick, (radius * 85) / 100, 0, second_color);
            fill_rect(gop, (UINTN)(cx - 3), (UINTN)(cy - 3), 7, 7, dial);
        }

        if (key_pressed(SystemTable)) {
            break;
        }

        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 100000);
    }

    fill_rect(gop, 0, 0, width, height, bg);
    return EFI_SUCCESS;
}

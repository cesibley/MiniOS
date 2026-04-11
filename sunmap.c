#include <efi.h>
#include <efilib.h>
#include "watchdog.h"
#include "sunmap_landmask.h"

#define PI_X1000000 3141593
#define TWILIGHT_COS_X1000 -120
#define MINUTES_PER_DAY 1440
#define SECONDS_PER_DAY 86400
#define SUNMAP_DECLINATION_GAIN_X100 100
#define SUNMAP_TERMINATOR_TERM1_GAIN_X100 35
#define SUNMAP_TERMINATOR_WAVE_GAIN_X100 10000
/*
 * Match gfxclock fallback behavior when firmware does not provide timezone.
 * Positive values are east of UTC; 420 means UTC-7 local display.
 */
#define SUNMAP_FALLBACK_TZ_MINUTES 420


static INTN sin_deg_x10000(INTN deg) {
    INTN x = deg % 360;
    INTN sign = 1;
    INT64 xr;
    INT64 pi = PI_X1000000;
    INT64 t;
    INT64 num;
    INT64 den;

    if (x < 0) {
        x += 360;
    }
    if (x > 180) {
        x -= 180;
        sign = -1;
    }

    /*
     * Bhaskara I sine approximation on [0, pi]:
     *   sin(x) ~= 16*x*(pi-x) / (5*pi^2 - 4*x*(pi-x))
     * with x in radians.
     * This is stable across the full range and avoids visible banding artifacts.
     */
    xr = ((INT64)x * pi) / 180;
    t = xr * (pi - xr);
    num = 16 * t;
    den = 5 * pi * pi - 4 * t;
    if (den == 0) {
        return 0;
    }

    return (INTN)(sign * ((num * 10000) / den));
}

static INTN cos_deg_x10000(INTN deg) {
    return sin_deg_x10000(90 - deg);
}

static INTN day_of_year(UINTN year, UINTN month, UINTN day) {
    static const UINTN month_days[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    UINTN m;
    UINTN total = day;
    BOOLEAN leap = ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0))) ? TRUE : FALSE;

    for (m = 0; m + 1 < month; m++) {
        total += month_days[m];
    }
    if (leap && month > 2) {
        total += 1;
    }
    return (INTN)total;
}

static INTN positive_mod(INTN value, INTN mod) {
    INTN out = value % mod;
    if (out < 0) {
        out += mod;
    }
    return out;
}

static VOID compute_local_time(const EFI_TIME *utc_now, EFI_TIME *local_now) {
    INTN tz_minutes;
    INTN local_minutes;
    INTN local_seconds;

    *local_now = *utc_now;

    if (utc_now->TimeZone == EFI_UNSPECIFIED_TIMEZONE) {
        tz_minutes = SUNMAP_FALLBACK_TZ_MINUTES;
    } else {
        tz_minutes = utc_now->TimeZone;
    }

    /*
     * UEFI timezone semantics: LocalTime = UTC - TimeZone.
     * Example: US Eastern Standard Time is +300.
     */
    local_minutes = ((INTN)utc_now->Hour * 60) + (INTN)utc_now->Minute - tz_minutes;
    local_minutes = positive_mod(local_minutes, MINUTES_PER_DAY);
    local_seconds = positive_mod((local_minutes * 60) + (INTN)utc_now->Second, SECONDS_PER_DAY);

    if ((utc_now->Daylight & EFI_TIME_ADJUST_DAYLIGHT) != 0 &&
        (utc_now->Daylight & EFI_TIME_IN_DAYLIGHT) != 0) {
        local_seconds = positive_mod(local_seconds + 3600, SECONDS_PER_DAY);
    }

    local_now->Hour = (UINT8)(local_seconds / 3600);
    local_now->Minute = (UINT8)((local_seconds % 3600) / 60);
    local_now->Second = (UINT8)(local_seconds % 60);
}

static BOOLEAN is_land(INTN lon_x10, INTN lat_x10) {
    INTN x = ((lon_x10 + 1800) * SUNMAP_MASK_WIDTH) / 3600;
    INTN y = ((900 - lat_x10) * SUNMAP_MASK_HEIGHT) / 1800;
    UINTN idx;

    if (x < 0) {
        x = 0;
    } else if (x >= SUNMAP_MASK_WIDTH) {
        x = SUNMAP_MASK_WIDTH - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= SUNMAP_MASK_HEIGHT) {
        y = SUNMAP_MASK_HEIGHT - 1;
    }

    idx = (UINTN)y * SUNMAP_MASK_STRIDE_BYTES + ((UINTN)x >> 3);
    return ((g_sunmap_land_mask[idx] >> (7 - (x & 7))) & 1U) != 0U;
}

static INTN sun_declination_deg(const EFI_TIME *utc_now) {
    INTN n = day_of_year(utc_now->Year, utc_now->Month, utc_now->Day);
    /* decl ~= -23.44 * cos(360*(N+10)/365) */
    INTN arg = ((n + 10) * 360) / 365;
    INTN decl = (-2344 * cos_deg_x10000(arg)) / 10000;

    /* Keep physical declination; wave exaggeration is handled separately. */
    decl = (decl * SUNMAP_DECLINATION_GAIN_X100) / 100;
    if (decl > 89) {
        decl = 89;
    } else if (decl < -89) {
        decl = -89;
    }
    return decl;
}

static INTN sun_longitude_deg_x10(const EFI_TIME *utc_now) {
    /* Subsolar longitude: 0 at 12:00 UTC. */
    INTN total_seconds = (INTN)utc_now->Hour * 3600 + (INTN)utc_now->Minute * 60 + (INTN)utc_now->Second;
    INTN lon_x10 = 1800 - (total_seconds / 24);

    while (lon_x10 > 1800) {
        lon_x10 -= 3600;
    }
    while (lon_x10 < -1800) {
        lon_x10 += 3600;
    }
    return lon_x10;
}

static INTN equation_of_time_min(const EFI_TIME *utc_now) {
    INTN n = day_of_year(utc_now->Year, utc_now->Month, utc_now->Day);
    INTN b = ((n - 81) * 360) / 364;
    INTN sin_b = sin_deg_x10000(b);
    INTN cos_b = cos_deg_x10000(b);
    INTN sin_2b = sin_deg_x10000(2 * b);

    /* 9.87*sin(2B)-7.53*cos(B)-1.5*sin(B), scaled by 10000 */
    INTN eot_x10000 = 98700 * sin_2b - 75300 * cos_b - 15000 * sin_b;
    return eot_x10000 / 100000000;
}

static INTN cos_solar_zenith_x1000(INTN lat_deg, INTN lon_deg, INTN subsolar_lat_deg, INTN subsolar_lon_deg) {
    INTN sin_lat = sin_deg_x10000(lat_deg);
    INTN cos_lat = cos_deg_x10000(lat_deg);
    INTN sin_dec = sin_deg_x10000(subsolar_lat_deg);
    INTN cos_dec = cos_deg_x10000(subsolar_lat_deg);
    INTN dlon = lon_deg - subsolar_lon_deg;
    INTN cos_dlon = cos_deg_x10000(dlon);
    INT64 term1 = (INT64)sin_lat * (INT64)sin_dec;
    INT64 term2 = ((INT64)cos_lat * (INT64)cos_dec / 10000) * (INT64)cos_dlon;
    INT64 c;

    /*
     * Artistic shaping:
     * - suppress latitude-bias term (term1) that flattens the line
     * - strongly boost longitude cosine wave (term2) to span map height
     */
    term1 = (term1 * SUNMAP_TERMINATOR_TERM1_GAIN_X100) / 100;
    term2 = (term2 * SUNMAP_TERMINATOR_WAVE_GAIN_X100) / 100;
    c = (term1 + term2) / 10000;
    if (c > 10000) {
        c = 10000;
    } else if (c < -10000) {
        c = -10000;
    }

    return (INTN)(c / 10);
}

static VOID fill_rect(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame,
                      UINTN width,
                      UINTN height,
                      UINTN x0,
                      UINTN y0,
                      UINTN rect_w,
                      UINTN rect_h,
                      UINT8 r,
                      UINT8 g,
                      UINT8 b) {
    UINTN y;
    UINTN y_end = y0 + rect_h;
    UINTN x_end = x0 + rect_w;

    if (x0 >= width || y0 >= height) {
        return;
    }
    if (x_end > width) {
        x_end = width;
    }
    if (y_end > height) {
        y_end = height;
    }

    for (y = y0; y < y_end; y++) {
        UINTN x;
        for (x = x0; x < x_end; x++) {
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px = &frame[y * width + x];
            px->Red = r;
            px->Green = g;
            px->Blue = b;
            px->Reserved = 0;
        }
    }
}

static UINT8 glyph_5x7(CHAR8 ch, UINTN row) {
    switch (ch) {
        case '0': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
            return g[row];
        }
        case '1': {
            static const UINT8 g[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
            return g[row];
        }
        case '2': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
            return g[row];
        }
        case '3': {
            static const UINT8 g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
            return g[row];
        }
        case '4': {
            static const UINT8 g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
            return g[row];
        }
        case '5': {
            static const UINT8 g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
            return g[row];
        }
        case '6': {
            static const UINT8 g[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case '7': {
            static const UINT8 g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
            return g[row];
        }
        case '8': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case '9': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
            return g[row];
        }
        case ':': {
            static const UINT8 g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
            return g[row];
        }
        case 'U': {
            static const UINT8 g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case 'T': {
            static const UINT8 g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
            return g[row];
        }
        case 'C': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
            return g[row];
        }
        case 'L': {
            static const UINT8 g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
            return g[row];
        }
        case 'O': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case 'A': {
            static const UINT8 g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
            return g[row];
        }
        default:
            return 0x00;
    }
}

static VOID draw_glyph(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame,
                       UINTN width,
                       UINTN height,
                       UINTN x,
                       UINTN y,
                       CHAR8 ch,
                       UINTN scale,
                       UINT8 r,
                       UINT8 g,
                       UINT8 b) {
    UINTN row;

    for (row = 0; row < 7; row++) {
        UINT8 bits = glyph_5x7(ch, row);
        UINTN col;
        for (col = 0; col < 5; col++) {
            if ((bits & (1U << (4 - col))) != 0) {
                fill_rect(frame, width, height,
                          x + col * scale, y + row * scale,
                          scale, scale,
                          r, g, b);
            }
        }
    }
}

static VOID draw_text(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame,
                      UINTN width,
                      UINTN height,
                      UINTN x,
                      UINTN y,
                      CONST CHAR8 *text,
                      UINTN scale,
                      UINT8 r,
                      UINT8 g,
                      UINT8 b) {
    UINTN i = 0;

    while (text[i] != '\0') {
        draw_glyph(frame, width, height, x + i * (6 * scale), y, text[i], scale, r, g, b);
        i++;
    }
}

static VOID format_time_line(CHAR8 *out, CONST CHAR8 *label, CONST EFI_TIME *t) {
    out[0] = label[0];
    out[1] = label[1];
    out[2] = label[2];
    out[3] = ' ';
    out[4] = (CHAR8)('0' + (t->Hour / 10));
    out[5] = (CHAR8)('0' + (t->Hour % 10));
    out[6] = ':';
    out[7] = (CHAR8)('0' + (t->Minute / 10));
    out[8] = (CHAR8)('0' + (t->Minute % 10));
    out[9] = ':';
    out[10] = (CHAR8)('0' + (t->Second / 10));
    out[11] = (CHAR8)('0' + (t->Second % 10));
    out[12] = '\0';
}

static VOID render_frame(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame,
                         UINTN width,
                         UINTN height,
                         INTN subsolar_lon_x10,
                         INTN subsolar_lat_deg,
                         CONST EFI_TIME *local_now,
                         CONST EFI_TIME *utc_now) {
    UINTN map_w = width;
    UINTN map_h = (width * 9) / 18;
    UINTN x_off = 0;
    UINTN y_off;
    UINTN y;

    if (map_h > height) {
        map_h = height;
        map_w = (height * 18) / 9;
        x_off = (width - map_w) / 2;
    }
    y_off = (height - map_h) / 2;

    for (y = 0; y < height; y++) {
        UINTN x;
        for (x = 0; x < width; x++) {
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL px;
            if (x < x_off || x >= x_off + map_w || y < y_off || y >= y_off + map_h) {
                px.Red = 0x00;
                px.Green = 0x00;
                px.Blue = 0x00;
                px.Reserved = 0;
            } else {
                INTN map_x = (INTN)(x - x_off);
                INTN map_y = (INTN)(y - y_off);
                INTN lon_x10 = (map_x * 3600) / (INTN)map_w - 1800;
                INTN lat_x10 = 900 - (map_y * 1800) / (INTN)map_h;
                INTN cosz = cos_solar_zenith_x1000(lat_x10 / 10,
                                                   lon_x10 / 10,
                                                   subsolar_lat_deg,
                                                   subsolar_lon_x10 / 10);
                BOOLEAN land = is_land(lon_x10, lat_x10);
                /* cosz > 0 means sun above horizon (day), cosz < 0 means night. */
                INTN day_strength = cosz;
                INTN brightness;
                INTN contrast_brightness;

                if (day_strength < TWILIGHT_COS_X1000) {
                    day_strength = TWILIGHT_COS_X1000;
                }
                brightness = ((day_strength - TWILIGHT_COS_X1000) * 1000) / (1000 - TWILIGHT_COS_X1000);
                if (brightness < 0) {
                    brightness = 0;
                } else if (brightness > 1000) {
                    brightness = 1000;
                }

                /* Increase global contrast while keeping twilight smooth. */
                contrast_brightness = ((brightness - 500) * 13) / 10 + 500;
                if (contrast_brightness < 0) {
                    contrast_brightness = 0;
                } else if (contrast_brightness > 1000) {
                    contrast_brightness = 1000;
                }
                brightness = contrast_brightness;

                if (land) {
                    INTN night_r = 34;
                    INTN night_g = 46;
                    INTN night_b = 24;
                    INTN day_r = 170;
                    INTN day_g = 218;
                    INTN day_b = 118;
                    px.Red = (UINT8)(night_r + ((day_r - night_r) * brightness) / 1000);
                    px.Green = (UINT8)(night_g + ((day_g - night_g) * brightness) / 1000);
                    px.Blue = (UINT8)(night_b + ((day_b - night_b) * brightness) / 1000);
                } else {
                    INTN night_r = 16;
                    INTN night_g = 26;
                    INTN night_b = 54;
                    INTN day_r = 90;
                    INTN day_g = 158;
                    INTN day_b = 228;
                    px.Red = (UINT8)(night_r + ((day_r - night_r) * brightness) / 1000);
                    px.Green = (UINT8)(night_g + ((day_g - night_g) * brightness) / 1000);
                    px.Blue = (UINT8)(night_b + ((day_b - night_b) * brightness) / 1000);
                }
                px.Reserved = 0;
            }
            frame[y * width + x] = px;
        }
    }

    {
        CHAR8 utc_line[13];
        CHAR8 local_line[13];
        UINTN scale = 2;
        UINTN text_w = 12 * 6 * scale;
        UINTN text_h = 2 * 7 * scale + 3 * scale;
        UINTN panel_x = x_off + 8;
        UINTN panel_y = y_off + 8;

        format_time_line(utc_line, "UTC", utc_now);
        format_time_line(local_line, "LOC", local_now);

        fill_rect(frame, width, height, panel_x, panel_y, text_w + 8, text_h + 8, 0, 0, 0);
        draw_text(frame, width, height, panel_x + 4, panel_y + 4, utc_line, scale, 255, 255, 255);
        draw_text(frame, width, height, panel_x + 4, panel_y + 4 + 8 * scale, local_line, scale, 255, 255, 255);
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame;
    UINTN width;
    UINTN height;
    UINTN frame_bytes;

    InitializeLib(ImageHandle, SystemTable);
    disable_uefi_watchdog(SystemTable);

    status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        Print(L"SUNMAP: GOP unavailable: %r\r\n", status);
        return status;
    }

    width = gop->Mode->Info->HorizontalResolution;
    height = gop->Mode->Info->VerticalResolution;
    frame_bytes = width * height * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, frame_bytes, (VOID **)&frame);
    if (EFI_ERROR(status) || frame == NULL) {
        Print(L"SUNMAP: frame allocation failed: %r\r\n", status);
        return status;
    }

    while (1) {
        EFI_TIME now_utc;
        EFI_TIME now_local;
        INTN subsolar_lon_x10;
        INTN subsolar_lat_deg;

        status = uefi_call_wrapper(SystemTable->RuntimeServices->GetTime, 2, &now_utc, NULL);
        if (EFI_ERROR(status)) {
            break;
        }

        compute_local_time(&now_utc, &now_local);
        subsolar_lon_x10 = sun_longitude_deg_x10(&now_utc);
        subsolar_lon_x10 -= (equation_of_time_min(&now_utc) * 10) / 4;
        while (subsolar_lon_x10 > 1800) {
            subsolar_lon_x10 -= 3600;
        }
        while (subsolar_lon_x10 < -1800) {
            subsolar_lon_x10 += 3600;
        }
        subsolar_lat_deg = sun_declination_deg(&now_utc);

        render_frame(frame, width, height, subsolar_lon_x10, subsolar_lat_deg, &now_local, &now_utc);

        uefi_call_wrapper(gop->Blt, 10,
                          gop,
                          frame,
                          EfiBltBufferToVideo,
                          0, 0,
                          0, 0,
                          width, height,
                          width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

        if (!EFI_ERROR(uefi_call_wrapper(SystemTable->BootServices->CheckEvent, 1,
                                         SystemTable->ConIn->WaitForKey))) {
            EFI_INPUT_KEY key;
            uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
            break;
        }

        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 200000);
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, frame);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);
    Print(L"SUNMAP exited.\r\n");

    return EFI_SUCCESS;
}

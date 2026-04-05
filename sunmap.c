#include <efi.h>
#include <efilib.h>
#include "watchdog.h"
#include "sunmap_landmask.h"

#define PI_X1000000 3141593
#define TWILIGHT_COS_X1000 -120
/*
 * If firmware reports EFI_UNSPECIFIED_TIMEZONE, treat it as Pacific local time
 * (including DST rules) and convert to UTC for solar calculations.
 */
#define SUNMAP_ASSUME_PACIFIC_IF_TZ_UNSPECIFIED 1


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

static UINTN days_in_month(UINTN year, UINTN month) {
    static const UINTN month_days[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    BOOLEAN leap = ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0))) ? TRUE : FALSE;
    UINTN d = month_days[month - 1];

    if (month == 2 && leap) {
        d += 1;
    }
    return d;
}

static UINTN day_of_week(UINTN year, UINTN month, UINTN day) {
    static const UINTN t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    UINTN y = year;
    if (month < 3) {
        y -= 1;
    }
    /* 0=Sunday .. 6=Saturday */
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

static INTN pacific_utc_offset_minutes(const EFI_TIME *t) {
    UINTN march_first_dow = day_of_week(t->Year, 3, 1);
    UINTN november_first_dow = day_of_week(t->Year, 11, 1);
    UINTN second_sunday_march = 1 + ((7 - march_first_dow) % 7) + 7;
    UINTN first_sunday_november = 1 + ((7 - november_first_dow) % 7);
    BOOLEAN in_dst = FALSE;

    if (t->Month > 3 && t->Month < 11) {
        in_dst = TRUE;
    } else if (t->Month == 3) {
        if (t->Day > second_sunday_march) {
            in_dst = TRUE;
        } else if (t->Day == second_sunday_march && t->Hour >= 2) {
            in_dst = TRUE;
        }
    } else if (t->Month == 11) {
        if (t->Day < first_sunday_november) {
            in_dst = TRUE;
        } else if (t->Day == first_sunday_november && t->Hour < 2) {
            in_dst = TRUE;
        }
    }

    /* local Pacific -> UTC offset (minutes to add to local clock) */
    return in_dst ? 420 : 480;
}

static VOID normalize_to_utc(EFI_TIME *t) {
    INTN tz_min;
    INTN total_seconds;

    if (t->TimeZone == EFI_UNSPECIFIED_TIMEZONE) {
        /* Unspecified timezone fallback is controlled by SUNMAP_ASSUME_PACIFIC_IF_TZ_UNSPECIFIED. */
#if SUNMAP_ASSUME_PACIFIC_IF_TZ_UNSPECIFIED
        tz_min = pacific_utc_offset_minutes(t);
#else
        tz_min = 0;
#endif
    } else {
        /*
         * EFI timezone is minutes west of UTC, so converting local -> UTC
         * means adding that many minutes.
         */
        tz_min = (INTN)t->TimeZone;
    }
    total_seconds = (INTN)t->Hour * 3600 + (INTN)t->Minute * 60 + (INTN)t->Second + tz_min * 60;

    while (total_seconds < 0) {
        UINTN dim;
        if (t->Day > 1) {
            t->Day -= 1;
        } else {
            if (t->Month > 1) {
                t->Month -= 1;
            } else {
                t->Month = 12;
                t->Year -= 1;
            }
            dim = days_in_month(t->Year, t->Month);
            t->Day = (UINT8)dim;
        }
        total_seconds += 24 * 3600;
    }

    while (total_seconds >= 24 * 3600) {
        UINTN dim = days_in_month(t->Year, t->Month);
        total_seconds -= 24 * 3600;
        if (t->Day < dim) {
            t->Day += 1;
        } else {
            t->Day = 1;
            if (t->Month < 12) {
                t->Month += 1;
            } else {
                t->Month = 1;
                t->Year += 1;
            }
        }
    }

    t->Hour = (UINT8)(total_seconds / 3600);
    t->Minute = (UINT8)((total_seconds % 3600) / 60);
    t->Second = (UINT8)(total_seconds % 60);
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
    return (-2344 * cos_deg_x10000(arg)) / 10000;
}

static INTN sun_longitude_deg_x10(const EFI_TIME *utc_now) {
    /* Subsolar longitude: 0 at 12:00 UTC. */
    INTN total_seconds = (INTN)utc_now->Hour * 3600 + (INTN)utc_now->Minute * 60 + (INTN)utc_now->Second;
    INTN lon_x10 = (total_seconds / 24) - 1800;

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
    INT64 c = (term1 + term2) / 10000;

    return (INTN)(c / 10);
}

static VOID render_frame(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame,
                         UINTN width,
                         UINTN height,
                         INTN subsolar_lon_x10,
                         INTN subsolar_lat_deg) {
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
                INTN day_strength = -cosz;
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
        INTN subsolar_lon_x10;
        INTN subsolar_lat_deg;

        status = uefi_call_wrapper(SystemTable->RuntimeServices->GetTime, 2, &now_utc, NULL);
        if (EFI_ERROR(status)) {
            break;
        }

        normalize_to_utc(&now_utc);
        subsolar_lon_x10 = sun_longitude_deg_x10(&now_utc);
        subsolar_lon_x10 += (equation_of_time_min(&now_utc) * 10) / 4;
        while (subsolar_lon_x10 > 1800) {
            subsolar_lon_x10 -= 3600;
        }
        while (subsolar_lon_x10 < -1800) {
            subsolar_lon_x10 += 3600;
        }
        subsolar_lat_deg = sun_declination_deg(&now_utc);

        render_frame(frame, width, height, subsolar_lon_x10, subsolar_lat_deg);

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

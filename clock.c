#include <efi.h>
#include <efilib.h>

static VOID wait_for_key(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN event_index;

    Print(L"\r\nPress any key to exit...");
    uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                      1, &SystemTable->ConIn->WaitForKey, &event_index);
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_TIME now;
    EFI_TIME_CAPABILITIES caps;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"CLOCK (UEFI RuntimeServices->GetTime)\r\n");

    status = uefi_call_wrapper(SystemTable->RuntimeServices->GetTime, 2, &now, &caps);
    if (EFI_ERROR(status)) {
        Print(L"GetTime failed: %r\r\n", status);
        wait_for_key(SystemTable);
        return status;
    }

    Print(L"\r\nCurrent UTC date/time:\r\n");
    Print(L"%04d-%02d-%02d %02d:%02d:%02d\r\n",
          now.Year, now.Month, now.Day,
          now.Hour, now.Minute, now.Second);

    Print(L"\r\nTime details:\r\n");
    Print(L"Timezone offset : %d minute(s)\r\n", now.TimeZone);
    Print(L"Daylight flags  : 0x%02x\r\n", now.Daylight);

    Print(L"\r\nClock capabilities:\r\n");
    Print(L"Resolution      : %u\r\n", caps.Resolution);
    Print(L"Accuracy        : %u\r\n", caps.Accuracy);
    Print(L"SetsToZero      : %u\r\n", caps.SetsToZero);

    wait_for_key(SystemTable);
    return EFI_SUCCESS;
}

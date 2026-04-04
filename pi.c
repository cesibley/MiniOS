#include <efi.h>
#include <efilib.h>

#define INPUT_MAX 32
#define MAX_DIGITS 5000

static EFI_STATUS read_line(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *buffer, UINTN max_len) {
    EFI_INPUT_KEY key;
    UINTN index = 0;
    UINTN event_index;

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);

        if (uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                              SystemTable->ConIn, &key) != EFI_SUCCESS) {
            continue;
        }

        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            buffer[index] = 0;
            Print(L"\r\n");
            return EFI_SUCCESS;
        }

        if (key.UnicodeChar == CHAR_BACKSPACE) {
            if (index > 0) {
                index--;
                buffer[index] = 0;
                Print(L"\b \b");
            }
            continue;
        }

        if (key.UnicodeChar >= L'0' && key.UnicodeChar <= L'9') {
            if (index < max_len - 1) {
                buffer[index++] = key.UnicodeChar;
                buffer[index] = 0;
                Print(L"%c", key.UnicodeChar);
            }
            continue;
        }
    }
}

static UINTN parse_uint(CHAR16 *text) {
    UINTN value = 0;

    while (*text) {
        if (*text < L'0' || *text > L'9') {
            return 0;
        }
        value = (value * 10) + (UINTN)(*text - L'0');
        text++;
    }
    return value;
}

static VOID print_digit(UINTN digit) {
    Print(L"%c", (CHAR16)(L'0' + digit));
}

static EFI_STATUS print_pi_digits(UINTN digits, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    UINTN len;
    UINTN *a = NULL;
    UINTN i, j;
    UINTN nines = 0;
    UINTN predigit = 0;

    if (digits == 0 || digits > MAX_DIGITS) {
        return EFI_INVALID_PARAMETER;
    }

    len = (digits * 10) / 3 + 1;
    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
                               EfiLoaderData, len * sizeof(UINTN), (VOID **)&a);
    if (EFI_ERROR(status)) {
        return status;
    }

    for (i = 0; i < len; i++) {
        a[i] = 2;
    }

    Print(L"\r\npi ~= 3.");
    for (j = 0; j < digits; j++) {
        UINTN q = 0;

        for (i = len; i > 0; i--) {
            UINTN x = 10 * a[i - 1] + q * i;
            UINTN denom = 2 * i - 1;
            a[i - 1] = x % denom;
            q = x / denom;
        }

        a[0] = q % 10;
        q = q / 10;

        if (q == 9) {
            nines++;
        } else if (q == 10) {
            print_digit(predigit + 1);
            while (nines > 0) {
                print_digit(0);
                nines--;
            }
            predigit = 0;
        } else {
            print_digit(predigit);
            predigit = q;
            while (nines > 0) {
                print_digit(9);
                nines--;
            }
        }
    }
    print_digit(predigit);
    Print(L"\r\n");

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, a);
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    CHAR16 input[INPUT_MAX];
    UINTN digits;
    EFI_STATUS status;
    (void)ImageHandle;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, 0, 0);

    Print(L"PI Spigot (UEFI)\r\n");
    Print(L"Decimal places to compute (1-%d): ", MAX_DIGITS);
    input[0] = 0;
    read_line(SystemTable, input, INPUT_MAX);
    digits = parse_uint(input);

    if (digits == 0 || digits > MAX_DIGITS) {
        Print(L"Invalid number of digits.\r\n");
        return EFI_INVALID_PARAMETER;
    }

    status = print_pi_digits(digits, SystemTable);
    if (EFI_ERROR(status)) {
        Print(L"Failed to compute pi: %r\r\n", status);
        return status;
    }

    Print(L"\r\nPress any key to exit...");
    {
        EFI_INPUT_KEY key;
        UINTN event_index;
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3,
                          1, &SystemTable->ConIn->WaitForKey, &event_index);
        uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
    }
    return EFI_SUCCESS;
}

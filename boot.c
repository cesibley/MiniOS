#include <efi.h>
#include <efilib.h>

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    Print(L"MiniOS UEFI booted successfully!\n");

    while (1); // halt

    return EFI_SUCCESS;
}

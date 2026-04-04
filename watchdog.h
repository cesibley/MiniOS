#ifndef MINIOS_WATCHDOG_H
#define MINIOS_WATCHDOG_H

#include <efi.h>

static inline VOID disable_uefi_watchdog(EFI_SYSTEM_TABLE *SystemTable) {
    if (SystemTable == NULL || SystemTable->BootServices == NULL) {
        return;
    }

    (VOID)uefi_call_wrapper(SystemTable->BootServices->SetWatchdogTimer, 5, 0, 0, 0, NULL);
}

#endif

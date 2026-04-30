#include <efi.h>
#include <efilib.h>

#define MAX_ITEMS 128
#define NAME_MAX 128
#define ICON_W 96
#define ICON_H 96
#define PAD 18
#define DOUBLE_CLICK_TICKS 4000000ULL

typedef struct {
    CHAR16 name[NAME_MAX];
    BOOLEAN is_dir;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL color;
} FILE_ITEM;

static UINTN to_col(UINTN idx, UINTN cols) { return idx % cols; }
static UINTN to_row(UINTN idx, UINTN cols) { return idx / cols; }

static VOID fill_rect(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL c) {
    uefi_call_wrapper(gop->Blt, 10, gop, &c, EfiBltVideoFill, 0, 0, x, y, w, h, 0);
}

static EFI_STATUS open_root(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_HANDLE *root) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status;
    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3, loaded_image->DeviceHandle, &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) return status;
    return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

static UINTN load_items(EFI_FILE_HANDLE root, FILE_ITEM *items) {
    UINTN count = 0;
    EFI_STATUS status;
    EFI_FILE_INFO *info;
    UINTN info_size = SIZE_OF_EFI_FILE_INFO + 512;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID **)&info);
    while (count < MAX_ITEMS) {
        SetMem(info, info_size, 0);
        UINTN sz = info_size;
        status = uefi_call_wrapper(root->Read, 3, root, &sz, info);
        if (EFI_ERROR(status) || sz == 0) break;
        if (StrCmp(info->FileName, L".") == 0 || StrCmp(info->FileName, L"..") == 0) continue;
        StrnCpy(items[count].name, info->FileName, NAME_MAX - 1);
        items[count].name[NAME_MAX - 1] = 0;
        items[count].is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        items[count].color.Blue = items[count].is_dir ? 0x10 : 0xA0;
        items[count].color.Green = items[count].is_dir ? 0x80 : 0xA0;
        items[count].color.Red = items[count].is_dir ? 0xF0 : 0x10;
        items[count].color.Reserved = 0;
        count++;
    }
    uefi_call_wrapper(BS->FreePool, 1, info);
    return count;
}

static VOID run_file(CHAR16 *path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_LOADED_IMAGE *loaded_image;
    EFI_DEVICE_PATH_PROTOCOL *file_path = NULL;
    EFI_STATUS status;
    EFI_HANDLE new_image = NULL;
    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) return;
    file_path = FileDevicePath(loaded_image->DeviceHandle, path);
    if (file_path == NULL) return;
    status = uefi_call_wrapper(SystemTable->BootServices->LoadImage, 6, FALSE, ImageHandle, file_path, NULL, 0, &new_image);
    if (!EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->StartImage, 3, new_image, NULL, NULL);
    }
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file_path);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    EFI_STATUS status;
    EFI_SIMPLE_POINTER_PROTOCOL *mouse;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_FILE_HANDLE root;
    FILE_ITEM items[MAX_ITEMS];
    UINTN count, cols, i;
    INTN mx = 60, my = 60;
    INTN selected = -1, last_selected = -1;
    EFI_TIME last_click = {0};

    status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID **)&gop);
    if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(BS->LocateProtocol, 3, &SimplePointerProtocol, NULL, (VOID **)&mouse);
    if (EFI_ERROR(status)) { Print(L"No mouse protocol\r\n"); return status; }
    uefi_call_wrapper(mouse->Reset, 2, mouse, TRUE);

    status = open_root(ImageHandle, SystemTable, &root);
    if (EFI_ERROR(status)) return status;
    count = load_items(root, items);
    uefi_call_wrapper(root->Close, 1, root);

    while (1) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg = {0x28,0x20,0x18,0};
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL cursor = {0xFF,0xFF,0xFF,0};
        EFI_SIMPLE_POINTER_STATE st;
        UINTN idx;
        fill_rect(gop, 0, 0, gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution, bg);
        cols = gop->Mode->Info->HorizontalResolution / (ICON_W + PAD);
        if (cols == 0) cols = 1;
        for (i = 0; i < count; i++) {
            UINTN x = PAD + to_col(i, cols) * (ICON_W + PAD);
            UINTN y = PAD + to_row(i, cols) * (ICON_H + 30);
            fill_rect(gop, x, y, ICON_W, ICON_H, items[i].color);
            ST->ConOut->SetCursorPosition(ST->ConOut, x / 8, (y + ICON_H + 2) / 16);
            Print(L"%s", items[i].name);
        }

        status = uefi_call_wrapper(mouse->GetState, 2, mouse, &st);
        if (!EFI_ERROR(status)) {
            mx += st.RelativeMovementX / 4;
            my -= st.RelativeMovementY / 4;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if ((UINTN)mx >= gop->Mode->Info->HorizontalResolution) mx = gop->Mode->Info->HorizontalResolution - 1;
            if ((UINTN)my >= gop->Mode->Info->VerticalResolution) my = gop->Mode->Info->VerticalResolution - 1;

            selected = -1;
            for (idx = 0; idx < count; idx++) {
                UINTN x = PAD + to_col(idx, cols) * (ICON_W + PAD);
                UINTN y = PAD + to_row(idx, cols) * (ICON_H + 30);
                if ((UINTN)mx >= x && (UINTN)mx <= x + ICON_W && (UINTN)my >= y && (UINTN)my <= y + ICON_H) { selected = (INTN)idx; break; }
            }
            if (st.LeftButton && selected >= 0) {
                EFI_TIME now;
                uefi_call_wrapper(ST->RuntimeServices->GetTime, 2, &now, NULL);
                if (last_selected == selected) {
                    UINT64 t1 = (UINT64)last_click.Second * 1000000ULL + last_click.Nanosecond / 1000ULL;
                    UINT64 t2 = (UINT64)now.Second * 1000000ULL + now.Nanosecond / 1000ULL;
                    UINT64 dt = (t2 >= t1) ? (t2 - t1) : (60000000ULL - t1 + t2);
                    if (dt < DOUBLE_CLICK_TICKS && !items[selected].is_dir) {
                        CHAR16 path[NAME_MAX + 2];
                        SPrint(path, sizeof(path), L"\\%s", items[selected].name);
                        run_file(path, ImageHandle, SystemTable);
                    }
                }
                last_selected = selected;
                last_click = now;
            }
        }
        fill_rect(gop, (UINTN)mx, (UINTN)my, 6, 6, cursor);
        uefi_call_wrapper(BS->Stall, 1, 16000);
    }
}

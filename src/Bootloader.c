#include <efi.h>
#include <efilib.h>

#define KERNEL_PATH  L"\\IzhOS\\System\\Kernel.efi"
#define COMMAND_PATH L"\\IzhOS\\System\\Command.efi"

static BOOLEAN IsFat32(EFI_HANDLE Handle)
{
    EFI_BLOCK_IO_PROTOCOL *Bio = NULL;
    EFI_STATUS Status;
    UINTN Size;
    UINT8 *Buffer;
    BOOLEAN Result = FALSE;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, Handle, &BlockIoProtocol, (VOID **)&Bio);
    if (EFI_ERROR(Status) || Bio == NULL || Bio->Media == NULL) {
        return FALSE;
    }
    if (!Bio->Media->MediaPresent) {
        return FALSE;
    }

    Size = Bio->Media->BlockSize;
    if (Size < 512) {
        return FALSE;
    }

    Buffer = AllocatePool(Size);
    if (Buffer == NULL) {
        return FALSE;
    }

    Status = uefi_call_wrapper(Bio->ReadBlocks, 5, Bio, Bio->Media->MediaId, (EFI_LBA)0, Size, Buffer);
    if (!EFI_ERROR(Status)) {
        if (Buffer[510] == 0x55 && Buffer[511] == 0xAA &&
            CompareMem(Buffer + 82, "FAT32   ", 8) == 0) {
            Result = TRUE;
            }
    }

    FreePool(Buffer);
    return Result;
}

static BOOLEAN FileExists(EFI_FILE_HANDLE Root, CHAR16 *Path)
{
    EFI_FILE_HANDLE File = NULL;
    EFI_STATUS Status;

    Status = uefi_call_wrapper(Root->Open, 5, Root, &File, Path, EFI_FILE_MODE_READ, (UINT64)0);
    if (EFI_ERROR(Status) || File == NULL) {
        return FALSE;
    }
    uefi_call_wrapper(File->Close, 1, File);
    return TRUE;
}

static BOOLEAN HasIzhOSLayout(EFI_HANDLE Handle)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_FILE_HANDLE Root = NULL;
    EFI_STATUS Status;
    BOOLEAN Result = FALSE;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, Handle, &FileSystemProtocol, (VOID **)&Fs);
    if (EFI_ERROR(Status) || Fs == NULL) {
        return FALSE;
    }

    Status = uefi_call_wrapper(Fs->OpenVolume, 2, Fs, &Root);
    if (EFI_ERROR(Status) || Root == NULL) {
        return FALSE;
    }

    if (FileExists(Root, KERNEL_PATH) && FileExists(Root, COMMAND_PATH)) {
        Result = TRUE;
    }

    uefi_call_wrapper(Root->Close, 1, Root);
    return Result;
}

static EFI_STATUS BootKernel(EFI_HANDLE Parent, EFI_HANDLE Device)
{
    EFI_DEVICE_PATH *Path;
    EFI_HANDLE Image = NULL;
    EFI_STATUS Status;
    UINTN ExitSize = 0;
    CHAR16 *ExitData = NULL;

    Path = FileDevicePath(Device, KERNEL_PATH);
    if (Path == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    Status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, Parent, Path, NULL, (UINTN)0, &Image);
    FreePool(Path);
    if (EFI_ERROR(Status)) {
        Print(L"IzhOS: failed to load Kernel.efi: %r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(BS->StartImage, 3, Image, &ExitSize, &ExitData);
    if (EFI_ERROR(Status)) {
        Print(L"IzhOS: Kernel.efi exited with: %r\n", Status);
    }
    return Status;
}

static VOID WaitKey(VOID)
{
    EFI_INPUT_KEY Key;

    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    uefi_call_wrapper(BS->WaitForEvent, 3, (UINTN)1, &ST->ConIn->WaitForKey, &(UINTN){0});
    uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_HANDLE *Handles = NULL;
    UINTN Count = 0;
    UINTN Index;
    EFI_STATUS Status;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, (UINTN)0, (UINT64)0, (UINTN)0, NULL);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    Print(L"IzhOS Bootloader\n");
    Print(L"Searching for IzhOS system partition...\n");

    Status = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &FileSystemProtocol, NULL, &Count, &Handles);
    if (EFI_ERROR(Status) || Count == 0) {
        Print(L"IzhOS: no file systems found: %r\n", Status);
        WaitKey();
        return EFI_NOT_FOUND;
    }

    for (Index = 0; Index < Count; Index++) {
        if (!IsFat32(Handles[Index])) {
            continue;
        }
        if (!HasIzhOSLayout(Handles[Index])) {
            continue;
        }

        Print(L"Found IzhOS system partition (%d). Starting kernel...\n", (INTN)Index);
        Status = BootKernel(ImageHandle, Handles[Index]);
        FreePool(Handles);
        WaitKey();
        return Status;
    }

    FreePool(Handles);
    Print(L"IzhOS: no FAT32 partition with a valid IzhOS layout was found.\n");
    Print(L"Press any key to return to firmware.\n");
    WaitKey();
    return EFI_NOT_FOUND;
}

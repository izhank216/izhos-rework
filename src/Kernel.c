#include <efi.h>
#include <efilib.h>

#define IZHOS_VERSION_MAJOR 1
#define IZHOS_VERSION_MINOR 0
#define COMMAND_PATH L"\\IzhOS\\System\\Command.efi"

#define IZHOS_KERNEL_GUID { 0x7a1c5e42, 0x3b9d, 0x4f08, { 0x9e, 0x61, 0x2d, 0xc4, 0xa8, 0x53, 0x17, 0xb0 } }

typedef struct {
    UINT32 Major;
    UINT32 Minor;
    EFI_HANDLE BootDevice;
    EFI_HANDLE KernelImage;
} IZHOS_KERNEL_INFO;

static EFI_GUID IzhOSKernelGuid = IZHOS_KERNEL_GUID;
static IZHOS_KERNEL_INFO KernelInfo;

static VOID WaitKey(VOID)
{
    UINTN Index;
    EFI_INPUT_KEY Key;

    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    uefi_call_wrapper(BS->WaitForEvent, 3, (UINTN)1, &ST->ConIn->WaitForKey, &Index);
    uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
}

static VOID SetupConsole(VOID)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *Out = ST->ConOut;
    UINTN Mode;
    UINTN Best = 0;
    UINTN BestArea = 0;
    UINTN Cols;
    UINTN Rows;

    for (Mode = 0; Mode < (UINTN)Out->Mode->MaxMode; Mode++) {
        if (EFI_ERROR(uefi_call_wrapper(Out->QueryMode, 4, Out, Mode, &Cols, &Rows))) {
            continue;
        }
        if (Cols * Rows > BestArea) {
            BestArea = Cols * Rows;
            Best = Mode;
        }
    }

    if (BestArea != 0) {
        uefi_call_wrapper(Out->SetMode, 2, Out, Best);
    }

    uefi_call_wrapper(Out->SetAttribute, 2, Out, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    uefi_call_wrapper(Out->ClearScreen, 1, Out);
    uefi_call_wrapper(Out->EnableCursor, 2, Out, TRUE);
}

static UINT64 CountMemory(UINT64 *FreePages)
{
    UINTN Entries = 0;
    UINTN MapKey = 0;
    UINTN DescSize = 0;
    UINT32 DescVersion = 0;
    EFI_MEMORY_DESCRIPTOR *Map;
    EFI_MEMORY_DESCRIPTOR *Desc;
    UINT64 Total = 0;
    UINTN Index;

    *FreePages = 0;

    Map = LibMemoryMap(&Entries, &MapKey, &DescSize, &DescVersion);
    if (Map == NULL) {
        return 0;
    }

    for (Index = 0; Index < Entries; Index++) {
        Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
        switch (Desc->Type) {
            case EfiReservedMemoryType:
            case EfiUnusableMemory:
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiPalCode:
                break;
            case EfiConventionalMemory:
                *FreePages += Desc->NumberOfPages;
                Total += Desc->NumberOfPages;
                break;
            default:
                Total += Desc->NumberOfPages;
                break;
        }
    }

    FreePool(Map);
    return Total;
}

static EFI_STATUS RegisterKernel(EFI_HANDLE Image, EFI_HANDLE Device)
{
    EFI_HANDLE Handle = NULL;

    KernelInfo.Major = IZHOS_VERSION_MAJOR;
    KernelInfo.Minor = IZHOS_VERSION_MINOR;
    KernelInfo.BootDevice = Device;
    KernelInfo.KernelImage = Image;

    return uefi_call_wrapper(BS->InstallProtocolInterface, 4, &Handle, &IzhOSKernelGuid, EFI_NATIVE_INTERFACE, &KernelInfo);
}

static VOID PrintBanner(VOID)
{
    UINT64 FreePages;
    UINT64 TotalPages;

    TotalPages = CountMemory(&FreePages);

    Print(L"IzhOS Kernel %d.%d\n", (INTN)IZHOS_VERSION_MAJOR, (INTN)IZHOS_VERSION_MINOR);
    Print(L"Firmware: %s (rev %d.%d)\n", ST->FirmwareVendor, (INTN)(ST->FirmwareRevision >> 16), (INTN)(ST->FirmwareRevision & 0xFFFF));
    Print(L"UEFI: %d.%d\n", (INTN)(ST->Hdr.Revision >> 16), (INTN)(ST->Hdr.Revision & 0xFFFF));
    Print(L"Memory: %lu MB total, %lu MB free\n", TotalPages / 256, FreePages / 256);
    Print(L"\n");
}

static EFI_STATUS RunShell(EFI_HANDLE Parent, EFI_HANDLE Device)
{
    EFI_DEVICE_PATH *Path;
    EFI_HANDLE Image = NULL;
    EFI_STATUS Status;
    UINTN ExitSize = 0;
    CHAR16 *ExitData = NULL;

    Path = FileDevicePath(Device, COMMAND_PATH);
    if (Path == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    Status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, Parent, Path, NULL, (UINTN)0, &Image);
    FreePool(Path);
    if (EFI_ERROR(Status)) {
        Print(L"Kernel: failed to load Command.efi: %r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(BS->StartImage, 3, Image, &ExitSize, &ExitData);
    if (ExitData != NULL) {
        FreePool(ExitData);
    }
    return Status;
}

static VOID Shutdown(VOID)
{
    Print(L"\nSystem is shutting down.\n");
    uefi_call_wrapper(RT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, (UINTN)0, NULL);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE *Loaded = NULL;
    EFI_STATUS Status;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, (UINTN)0, (UINT64)0, (UINTN)0, NULL);

    SetupConsole();

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&Loaded);
    if (EFI_ERROR(Status) || Loaded == NULL || Loaded->DeviceHandle == NULL) {
        Print(L"Kernel panic: cannot determine boot device: %r\n", Status);
        WaitKey();
        return EFI_LOAD_ERROR;
    }

    Status = RegisterKernel(ImageHandle, Loaded->DeviceHandle);
    if (EFI_ERROR(Status)) {
        Print(L"Kernel panic: cannot register kernel services: %r\n", Status);
        WaitKey();
        return Status;
    }

    PrintBanner();

    for (;;) {
        Status = RunShell(ImageHandle, Loaded->DeviceHandle);

        if (!EFI_ERROR(Status)) {
            Shutdown();
            WaitKey();
            return EFI_SUCCESS;
        }

        Print(L"Kernel: shell terminated with status: %r\n", Status);
        Print(L"Press any key to restart the shell.\n");
        WaitKey();
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    }
}

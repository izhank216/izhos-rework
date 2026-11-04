#include <efi.h>
#include <efilib.h>

#define MAXDISKS 32
#define MAXPARTS 128
#define MAXLINE 256
#define MAXARGS 8

typedef struct {
    EFI_HANDLE H;
    EFI_BLOCK_IO *Bio;
    UINTN Parts;
} DiskInfo;

typedef struct {
    EFI_HANDLE H;
    EFI_BLOCK_IO *Bio;
    UINTN Disk;
    UINT32 Num;
    UINT64 Start;
    UINT64 Size;
} PartInfo;

typedef struct {
    EFI_HANDLE H;
    EFI_BLOCK_IO *Bio;
    UINTN Disk;
    BOOLEAN IsPart;
    UINT32 Num;
    UINT64 Start;
} TargetInfo;

static DiskInfo gDisks[MAXDISKS];
static PartInfo gParts[MAXPARTS];
static UINTN gDiskCount;
static UINTN gPartCount;
static BOOLEAN gYes;
static BOOLEAN gQuick;

static UINT16 Get16(UINT8 *B, UINTN O)
{
    return (UINT16)(B[O] | (B[O + 1] << 8));
}

static UINT32 Get32(UINT8 *B, UINTN O)
{
    return (UINT32)B[O] | ((UINT32)B[O + 1] << 8) | ((UINT32)B[O + 2] << 16) | ((UINT32)B[O + 3] << 24);
}

static UINT64 Get64(UINT8 *B, UINTN O)
{
    return (UINT64)Get32(B, O) | ((UINT64)Get32(B, O + 4) << 32);
}

static VOID Put16(UINT8 *B, UINTN O, UINT16 V)
{
    B[O] = (UINT8)V;
    B[O + 1] = (UINT8)(V >> 8);
}

static VOID Put32(UINT8 *B, UINTN O, UINT32 V)
{
    B[O] = (UINT8)V;
    B[O + 1] = (UINT8)(V >> 8);
    B[O + 2] = (UINT8)(V >> 16);
    B[O + 3] = (UINT8)(V >> 24);
}

static UINT32 Crc32(UINT8 *D, UINTN N)
{
    UINT32 C = 0xFFFFFFFF;
    UINTN I;
    INTN K;

    for (I = 0; I < N; I++) {
        C ^= D[I];
        for (K = 0; K < 8; K++) {
            C = (C >> 1) ^ (0xEDB88320 & (0 - (C & 1)));
        }
    }
    return ~C;
}

static EFI_STATUS Rd(EFI_BLOCK_IO *B, UINT64 Lba, UINTN Bytes, VOID *Buf)
{
    return uefi_call_wrapper(B->ReadBlocks, 5, B, B->Media->MediaId, Lba, Bytes, Buf);
}

static EFI_STATUS Wr(EFI_BLOCK_IO *B, UINT64 Lba, UINTN Bytes, VOID *Buf)
{
    return uefi_call_wrapper(B->WriteBlocks, 5, B, B->Media->MediaId, Lba, Bytes, Buf);
}

static VOID Flush(EFI_BLOCK_IO *B)
{
    uefi_call_wrapper(B->FlushBlocks, 1, B);
}

static EFI_INPUT_KEY GetKey(VOID)
{
    EFI_INPUT_KEY K;
    UINTN Index;

    for (;;) {
        if (!EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &K))) {
            return K;
        }
        uefi_call_wrapper(BS->WaitForEvent, 3, (UINTN)1, &ST->ConIn->WaitForKey, &Index);
    }
}

static BOOLEAN Confirm(CONST CHAR16 *What)
{
    EFI_INPUT_KEY K;

    if (gYes) {
        return TRUE;
    }
    Print(L"%s - continue? [y/N] ", What);
    K = GetKey();
    Print(L"%c\n", K.UnicodeChar >= 32 ? K.UnicodeChar : L' ');
    return K.UnicodeChar == L'y' || K.UnicodeChar == L'Y';
}

static VOID SizeStr(UINT64 B, CHAR16 *Out)
{
    if (B >= ((UINT64)1 << 30)) {
        SPrint(Out, 48, L"%lu.%02lu GB", B >> 30, ((B & 0x3FFFFFFF) * 100) >> 30);
    } else if (B >= ((UINT64)1 << 20)) {
        SPrint(Out, 48, L"%lu.%02lu MB", B >> 20, ((B & 0xFFFFF) * 100) >> 20);
    } else {
        SPrint(Out, 48, L"%lu KB", B >> 10);
    }
}

static BOOLEAN PathInfo(EFI_DEVICE_PATH *Dp, EFI_DEVICE_PATH **Node, UINT32 *Num, UINT64 *Start, UINT64 *Size)
{
    EFI_DEVICE_PATH *N = Dp;
    HARDDRIVE_DEVICE_PATH *Hd;
    CDROM_DEVICE_PATH *Cd;

    while (!IsDevicePathEnd(N)) {
        if (DevicePathType(N) == MEDIA_DEVICE_PATH) {
            if (DevicePathSubType(N) == MEDIA_HARDDRIVE_DP) {
                Hd = (HARDDRIVE_DEVICE_PATH *)N;
                *Node = N;
                *Num = Hd->PartitionNumber;
                *Start = Hd->PartitionStart;
                *Size = Hd->PartitionSize;
                return TRUE;
            }
            if (DevicePathSubType(N) == MEDIA_CDROM_DP) {
                Cd = (CDROM_DEVICE_PATH *)N;
                *Node = N;
                *Num = Cd->BootEntry + 1;
                *Start = Cd->PartitionStart;
                *Size = Cd->PartitionSize;
                return TRUE;
            }
        }
        N = NextDevicePathNode(N);
    }
    return FALSE;
}

static UINTN DpLen(EFI_DEVICE_PATH *Dp)
{
    return DevicePathSize(Dp) - sizeof(EFI_DEVICE_PATH);
}

static BOOLEAN GetBio(EFI_HANDLE H, EFI_BLOCK_IO **B)
{
    *B = NULL;
    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, H, &BlockIoProtocol, (VOID **)B))) {
        return FALSE;
    }
    return *B != NULL && (*B)->Media != NULL;
}

static VOID Scan(VOID)
{
    EFI_HANDLE *H = NULL;
    UINTN N = 0;
    UINTN I;
    UINTN J;
    UINTN Prefix;
    EFI_BLOCK_IO *B;
    EFI_DEVICE_PATH *Dp;
    EFI_DEVICE_PATH *Dd;
    EFI_DEVICE_PATH *Node;
    PartInfo Tmp;
    UINT32 Num;
    UINT64 Start;
    UINT64 Size;

    gDiskCount = 0;
    gPartCount = 0;
    if (EFI_ERROR(uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &BlockIoProtocol, NULL, &N, &H)) || H == NULL) {
        return;
    }

    for (I = 0; I < N; I++) {
        if (!GetBio(H[I], &B) || B->Media->LogicalPartition || gDiskCount >= MAXDISKS) {
            continue;
        }
        gDisks[gDiskCount].H = H[I];
        gDisks[gDiskCount].Bio = B;
        gDisks[gDiskCount].Parts = 0;
        gDiskCount++;
    }

    for (I = 0; I < N; I++) {
        if (!GetBio(H[I], &B) || !B->Media->LogicalPartition || gPartCount >= MAXPARTS) {
            continue;
        }
        Dp = DevicePathFromHandle(H[I]);
        if (Dp == NULL || !PathInfo(Dp, &Node, &Num, &Start, &Size)) {
            continue;
        }
        Prefix = (UINTN)((UINT8 *)Node - (UINT8 *)Dp);
        for (J = 0; J < gDiskCount; J++) {
            Dd = DevicePathFromHandle(gDisks[J].H);
            if (Dd != NULL && DpLen(Dd) == Prefix && !CompareMem(Dd, Dp, Prefix)) {
                gParts[gPartCount].H = H[I];
                gParts[gPartCount].Bio = B;
                gParts[gPartCount].Disk = J;
                gParts[gPartCount].Num = Num;
                gParts[gPartCount].Start = Start;
                gParts[gPartCount].Size = Size;
                gPartCount++;
                gDisks[J].Parts++;
                break;
            }
        }
    }
    FreePool(H);

    for (I = 1; I < gPartCount; I++) {
        Tmp = gParts[I];
        for (J = I; J > 0 && (gParts[J - 1].Disk > Tmp.Disk || (gParts[J - 1].Disk == Tmp.Disk && gParts[J - 1].Num > Tmp.Num)); J--) {
            gParts[J] = gParts[J - 1];
        }
        gParts[J] = Tmp;
    }
}

static CONST CHAR16 *FsName(EFI_BLOCK_IO *B)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT8 *S;
    CONST CHAR16 *R = L"RAW";

    if (!B->Media->MediaPresent || Bs < 512 || Bs > 65536) {
        return L"-";
    }
    S = AllocatePool(Bs);
    if (S == NULL) {
        return L"-";
    }
    if (EFI_ERROR(Rd(B, 0, Bs, S))) {
        R = L"-";
    } else if (!CompareMem(S + 82, "FAT32   ", 8)) {
        R = L"FAT32";
    } else if (!CompareMem(S + 54, "FAT16   ", 8)) {
        R = L"FAT16";
    } else if (!CompareMem(S + 54, "FAT12   ", 8)) {
        R = L"FAT12";
    } else if (!CompareMem(S + 3, "NTFS    ", 8)) {
        R = L"NTFS";
    } else if (!CompareMem(S + 3, "EXFAT   ", 8)) {
        R = L"exFAT";
    }
    FreePool(S);
    return R;
}

static CONST CHAR16 *TableName(EFI_BLOCK_IO *B)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT8 *S;
    CONST CHAR16 *R = L"None";
    UINTN I;
    BOOLEAN Fat;
    BOOLEAN Used = FALSE;

    if (!B->Media->MediaPresent || Bs < 512 || Bs > 65536) {
        return L"-";
    }
    S = AllocatePool(Bs);
    if (S == NULL) {
        return L"-";
    }
    if (!EFI_ERROR(Rd(B, 1, Bs, S)) && !CompareMem(S, "EFI PART", 8)) {
        R = L"GPT";
    } else if (!EFI_ERROR(Rd(B, 0, Bs, S)) && S[510] == 0x55 && S[511] == 0xAA) {
        Fat = (S[16] == 1 || S[16] == 2) && Get16(S, 11) == Bs;
        for (I = 0; I < 4; I++) {
            if (S[446 + 16 * I + 4] != 0) {
                Used = TRUE;
            }
        }
        if (!Fat && Used) {
            R = L"MBR";
        }
    }
    FreePool(S);
    return R;
}

static BOOLEAN ParseTarget(CHAR16 *S, UINTN *D, INTN *P)
{
    UINTN V = 0;
    BOOLEAN Any = FALSE;

    while (*S >= L'0' && *S <= L'9') {
        V = V * 10 + (UINTN)(*S - L'0');
        S++;
        Any = TRUE;
    }
    if (!Any) {
        return FALSE;
    }
    *D = V;
    *P = -1;
    if (*S == 0) {
        return TRUE;
    }
    if (*S != L':') {
        return FALSE;
    }
    S++;
    V = 0;
    Any = FALSE;
    while (*S >= L'0' && *S <= L'9') {
        V = V * 10 + (UINTN)(*S - L'0');
        S++;
        Any = TRUE;
    }
    if (!Any || *S != 0) {
        return FALSE;
    }
    *P = (INTN)V;
    return TRUE;
}

static BOOLEAN GetTarget(CHAR16 *Arg, TargetInfo *T)
{
    UINTN D;
    UINTN I;
    INTN P;

    if (!ParseTarget(Arg, &D, &P) || D >= gDiskCount) {
        Print(L"Invalid target. Use <disk> or <disk>:<part> (see listdisk, listpar)\n");
        return FALSE;
    }
    T->Disk = D;
    T->IsPart = FALSE;
    T->Num = 0;
    T->Start = 0;
    T->H = gDisks[D].H;
    T->Bio = gDisks[D].Bio;
    if (P < 0) {
        return TRUE;
    }
    for (I = 0; I < gPartCount; I++) {
        if (gParts[I].Disk == D && gParts[I].Num == (UINT32)P) {
            T->IsPart = TRUE;
            T->Num = gParts[I].Num;
            T->Start = gParts[I].Start;
            T->H = gParts[I].H;
            T->Bio = gParts[I].Bio;
            return TRUE;
        }
    }
    Print(L"Partition %d:%d not found\n", (INTN)D, P);
    return FALSE;
}

static VOID Cell(CONST CHAR16 *S, UINTN W)
{
    UINTN L = StrLen((CHAR16 *)S);

    Print(L"%s", S);
    while (L++ < W) {
        Print(L" ");
    }
}

static VOID ConnectAll(VOID)
{
    EFI_HANDLE *H = NULL;
    UINTN N = 0;
    UINTN I;

    if (EFI_ERROR(uefi_call_wrapper(BS->LocateHandleBuffer, 5, AllHandles, NULL, NULL, &N, &H)) || H == NULL) {
        return;
    }
    for (I = 0; I < N; I++) {
        uefi_call_wrapper(BS->ConnectController, 4, H[I], NULL, NULL, TRUE);
    }
    FreePool(H);
}

static VOID StopFs(EFI_HANDLE H)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *E = NULL;
    UINTN N = 0;
    UINTN I;

    if (EFI_ERROR(uefi_call_wrapper(BS->OpenProtocolInformation, 4, H, &DiskIoProtocol, &E, &N)) || E == NULL) {
        return;
    }
    for (I = 0; I < N; I++) {
        if ((E[I].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) && E[I].AgentHandle != NULL) {
            uefi_call_wrapper(BS->DisconnectController, 3, H, E[I].AgentHandle, NULL);
        }
    }
    FreePool(E);
}

static VOID Release(UINTN Disk)
{
    UINTN I;

    for (I = 0; I < gPartCount; I++) {
        if (gParts[I].Disk == Disk) {
            StopFs(gParts[I].H);
        }
    }
    StopFs(gDisks[Disk].H);
}

static VOID CmdListDisk(VOID)
{
    UINTN I;
    CHAR16 Sz[48];
    CHAR16 Num[24];
    EFI_BLOCK_IO_MEDIA *M;
    CONST CHAR16 *Type;

    if (gDiskCount == 0) {
        Print(L"No disks found\n");
        return;
    }
    Print(L"Disk  Size          Block  Type          Table  Parts\n");
    for (I = 0; I < gDiskCount; I++) {
        M = gDisks[I].Bio->Media;
        if (M->MediaPresent) {
            SizeStr((M->LastBlock + 1) * M->BlockSize, Sz);
        } else {
            SPrint(Sz, sizeof(Sz), L"No media");
        }
        if (M->ReadOnly) {
            Type = M->RemovableMedia ? L"Removable RO" : L"Fixed RO";
        } else {
            Type = M->RemovableMedia ? L"Removable" : L"Fixed";
        }
        SPrint(Num, sizeof(Num), L"%d", (INTN)I);
        Cell(Num, 6);
        Cell(Sz, 14);
        SPrint(Num, sizeof(Num), L"%d", (INTN)M->BlockSize);
        Cell(Num, 7);
        Cell(Type, 14);
        Cell(TableName(gDisks[I].Bio), 7);
        Print(L"%d\n", (INTN)gDisks[I].Parts);
    }
}

static VOID CmdListPar(INTN Argc, CHAR16 **Argv)
{
    UINTN D = 0;
    UINTN Di;
    UINTN I;
    INTN P;
    BOOLEAN All = TRUE;
    CHAR16 Sz[48];
    CHAR16 Num[24];

    if (Argc > 1) {
        if (!ParseTarget(Argv[1], &D, &P) || P != -1 || D >= gDiskCount) {
            Print(L"Invalid disk\n");
            return;
        }
        All = FALSE;
    }
    for (Di = 0; Di < gDiskCount; Di++) {
        if (!All && Di != D) {
            continue;
        }
        Print(L"Disk %d  table: %s  partitions: %d\n", (INTN)Di, TableName(gDisks[Di].Bio), (INTN)gDisks[Di].Parts);
        if (gDisks[Di].Parts == 0) {
            Print(L"  no partitions, file system: %s\n", FsName(gDisks[Di].Bio));
            continue;
        }
        Print(L"  Part  Start LBA     Size          FS\n");
        for (I = 0; I < gPartCount; I++) {
            if (gParts[I].Disk != Di) {
                continue;
            }
            SizeStr(gParts[I].Size * gDisks[Di].Bio->Media->BlockSize, Sz);
            Print(L"  ");
            SPrint(Num, sizeof(Num), L"%d", (INTN)gParts[I].Num);
            Cell(Num, 6);
            SPrint(Num, sizeof(Num), L"%lu", gParts[I].Start);
            Cell(Num, 14);
            Cell(Sz, 14);
            Print(L"%s\n", FsName(gParts[I].Bio));
        }
    }
}

static VOID MountOne(EFI_HANDLE H, UINTN Disk, INTN Num)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_FILE_HANDLE Root = NULL;
    EFI_FILE_SYSTEM_INFO *Info;
    CHAR16 Name[24];
    CHAR16 A[48];
    CHAR16 B[48];

    if (Num < 0) {
        SPrint(Name, sizeof(Name), L"%d", (INTN)Disk);
    } else {
        SPrint(Name, sizeof(Name), L"%d:%d", (INTN)Disk, Num);
    }
    uefi_call_wrapper(BS->ConnectController, 4, H, NULL, NULL, TRUE);
    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, H, &FileSystemProtocol, (VOID **)&Fs)) || Fs == NULL) {
        Print(L"%s  no mountable file system\n", Name);
        return;
    }
    if (EFI_ERROR(uefi_call_wrapper(Fs->OpenVolume, 2, Fs, &Root)) || Root == NULL) {
        Print(L"%s  file system present but cannot open volume\n", Name);
        return;
    }
    Info = LibFileSystemInfo(Root);
    if (Info != NULL) {
        SizeStr(Info->VolumeSize, A);
        SizeStr(Info->FreeSpace, B);
        Print(L"%s  mounted  label \"%s\"  size %s  free %s\n", Name, Info->VolumeLabel, A, B);
        FreePool(Info);
    } else {
        Print(L"%s  mounted\n", Name);
    }
    uefi_call_wrapper(Root->Close, 1, Root);
}

static VOID CmdMount(INTN Argc, CHAR16 **Argv)
{
    TargetInfo T;
    UINTN I;
    UINTN Found = 0;

    if (Argc < 2) {
        Print(L"Usage: mount <disk>[:<part>]\n");
        return;
    }
    if (!GetTarget(Argv[1], &T)) {
        return;
    }
    if (T.IsPart) {
        MountOne(T.H, T.Disk, (INTN)T.Num);
        return;
    }
    uefi_call_wrapper(BS->ConnectController, 4, T.H, NULL, NULL, TRUE);
    Scan();
    for (I = 0; I < gPartCount; I++) {
        if (gParts[I].Disk == T.Disk) {
            MountOne(gParts[I].H, T.Disk, (INTN)gParts[I].Num);
            Found++;
        }
    }
    if (Found == 0) {
        MountOne(gDisks[T.Disk].H, T.Disk, -1);
    }
}

static VOID CmdUnmount(INTN Argc, CHAR16 **Argv)
{
    TargetInfo T;

    if (Argc < 2) {
        Print(L"Usage: unmount <disk>[:<part>]\n");
        return;
    }
    if (!GetTarget(Argv[1], &T)) {
        return;
    }
    if (T.IsPart) {
        StopFs(T.H);
    } else {
        Release(T.Disk);
    }
    Print(L"Unmounted %s\n", Argv[1]);
}

static EFI_STATUS GptFix(EFI_BLOCK_IO *B, UINT64 HdrLba, UINT32 Num)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT8 *H = AllocatePool(Bs);
    UINT8 *T = NULL;
    UINT32 HSize;
    UINT32 NumEnt;
    UINT32 EntSize;
    UINT32 Bytes;
    UINT32 Sectors;
    UINT64 EntLba;
    EFI_STATUS S;

    if (H == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    S = Rd(B, HdrLba, Bs, H);
    if (EFI_ERROR(S)) {
        goto done;
    }
    if (CompareMem(H, "EFI PART", 8)) {
        S = EFI_NOT_FOUND;
        goto done;
    }
    HSize = Get32(H, 12);
    EntLba = Get64(H, 72);
    NumEnt = Get32(H, 80);
    EntSize = Get32(H, 84);
    if (Num < 1 || Num > NumEnt) {
        S = EFI_NOT_FOUND;
        goto done;
    }
    if (HSize < 92 || HSize > Bs || EntSize < 128 || EntSize > 4096 || NumEnt > 4096) {
        S = EFI_VOLUME_CORRUPTED;
        goto done;
    }
    Bytes = NumEnt * EntSize;
    Sectors = (Bytes + Bs - 1) / Bs;
    T = AllocatePool((UINTN)Sectors * Bs);
    if (T == NULL) {
        S = EFI_OUT_OF_RESOURCES;
        goto done;
    }
    S = Rd(B, EntLba, (UINTN)Sectors * Bs, T);
    if (EFI_ERROR(S)) {
        goto done;
    }
    ZeroMem(T + (UINTN)(Num - 1) * EntSize, EntSize);
    Put32(H, 88, Crc32(T, Bytes));
    Put32(H, 16, 0);
    Put32(H, 16, Crc32(H, HSize));
    S = Wr(B, EntLba, (UINTN)Sectors * Bs, T);
    if (!EFI_ERROR(S)) {
        S = Wr(B, HdrLba, Bs, H);
    }
done:
    if (T != NULL) {
        FreePool(T);
    }
    FreePool(H);
    return S;
}

static EFI_STATUS DeletePart(EFI_BLOCK_IO *B, UINT32 Num)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT8 *S;
    UINT64 Backup;
    EFI_STATUS St;

    if (Bs < 512 || Bs > 65536) {
        return EFI_UNSUPPORTED;
    }
    S = AllocatePool(Bs);
    if (S == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (!EFI_ERROR(Rd(B, 1, Bs, S)) && !CompareMem(S, "EFI PART", 8)) {
        Backup = Get64(S, 32);
        FreePool(S);
        St = GptFix(B, 1, Num);
        if (!EFI_ERROR(St) && Backup > 1 && Backup <= B->Media->LastBlock) {
            St = GptFix(B, Backup, Num);
        }
        return St;
    }
    St = Rd(B, 0, Bs, S);
    if (!EFI_ERROR(St)) {
        if (S[510] != 0x55 || S[511] != 0xAA) {
            St = EFI_NOT_FOUND;
        } else if (Num < 1 || Num > 4) {
            St = EFI_UNSUPPORTED;
        } else {
            ZeroMem(S + 446 + 16 * (Num - 1), 16);
            St = Wr(B, 0, Bs, S);
        }
    }
    FreePool(S);
    return St;
}

static VOID CmdDelete(INTN Argc, CHAR16 **Argv)
{
    TargetInfo T;
    EFI_STATUS S;
    CHAR16 Msg[64];
    DiskInfo *D;

    if (Argc < 2) {
        Print(L"Usage: delete <disk>:<part> [/y]\n");
        return;
    }
    if (!GetTarget(Argv[1], &T)) {
        return;
    }
    if (!T.IsPart) {
        Print(L"Specify a partition: <disk>:<part>\n");
        return;
    }
    D = &gDisks[T.Disk];
    if (D->Bio->Media->ReadOnly) {
        Print(L"Disk is read-only\n");
        return;
    }
    SPrint(Msg, sizeof(Msg), L"Delete partition %d:%d", (INTN)T.Disk, (INTN)T.Num);
    if (!Confirm(Msg)) {
        Print(L"Aborted\n");
        return;
    }
    Release(T.Disk);
    S = DeletePart(D->Bio, T.Num);
    Flush(D->Bio);
    uefi_call_wrapper(BS->ConnectController, 4, D->H, NULL, NULL, TRUE);
    if (EFI_ERROR(S)) {
        Print(L"delete failed: %r\n", S);
    } else {
        Print(L"Deleted partition %d:%d\n", (INTN)T.Disk, (INTN)T.Num);
    }
}

static EFI_STATUS ZeroRange(EFI_BLOCK_IO *B, UINT64 Lba, UINT64 Count, BOOLEAN Show)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT64 Chunk = (UINT64)1048576 / Bs;
    UINT64 Done = 0;
    UINT64 N;
    UINT8 *Buf;
    UINTN Pct;
    UINTN Last = 101;
    EFI_STATUS S = EFI_SUCCESS;

    if (Chunk == 0) {
        Chunk = 1;
    }
    Buf = AllocateZeroPool((UINTN)(Chunk * Bs));
    if (Buf == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    while (Done < Count) {
        N = Count - Done;
        if (N > Chunk) {
            N = Chunk;
        }
        S = Wr(B, Lba + Done, (UINTN)(N * Bs), Buf);
        if (EFI_ERROR(S)) {
            break;
        }
        Done += N;
        if (Show) {
            Pct = (UINTN)((Done * 100) / Count);
            if (Pct != Last) {
                Last = Pct;
                Print(L"\r%d%%", (INTN)Pct);
            }
        }
    }
    if (Show) {
        Print(L"\n");
    }
    FreePool(Buf);
    return S;
}

static VOID CmdZero(INTN Argc, CHAR16 **Argv)
{
    TargetInfo T;
    EFI_BLOCK_IO *B;
    UINT64 Total;
    UINT64 Edge;
    EFI_STATUS S;
    CHAR16 Msg[64];

    if (Argc < 2) {
        Print(L"Usage: zero <disk> [/q] [/y]\n");
        return;
    }
    if (!GetTarget(Argv[1], &T)) {
        return;
    }
    if (T.IsPart) {
        Print(L"zero works on whole disks: zero <disk>\n");
        return;
    }
    B = T.Bio;
    if (!B->Media->MediaPresent) {
        Print(L"No media\n");
        return;
    }
    if (B->Media->ReadOnly) {
        Print(L"Disk is read-only\n");
        return;
    }
    Total = B->Media->LastBlock + 1;
    SPrint(Msg, sizeof(Msg), gQuick ? L"Wipe partition tables of disk %d" : L"Erase all data on disk %d", (INTN)T.Disk);
    if (!Confirm(Msg)) {
        Print(L"Aborted\n");
        return;
    }
    Release(T.Disk);
    if (gQuick) {
        Edge = (UINT64)1048576 / B->Media->BlockSize;
        if (Edge == 0) {
            Edge = 1;
        }
        if (Edge * 2 >= Total) {
            S = ZeroRange(B, 0, Total, FALSE);
        } else {
            S = ZeroRange(B, 0, Edge, FALSE);
            if (!EFI_ERROR(S)) {
                S = ZeroRange(B, Total - Edge, Edge, FALSE);
            }
        }
    } else {
        S = ZeroRange(B, 0, Total, TRUE);
    }
    Flush(B);
    uefi_call_wrapper(BS->ConnectController, 4, T.H, NULL, NULL, TRUE);
    if (EFI_ERROR(S)) {
        Print(L"zero failed: %r\n", S);
    } else {
        Print(L"Disk %d zeroed\n", (INTN)T.Disk);
    }
}

static EFI_STATUS Fat32(EFI_BLOCK_IO *B, UINT64 StartLba, CONST CHAR16 *Label)
{
    UINT32 Bs = B->Media->BlockSize;
    UINT64 Total = B->Media->LastBlock + 1;
    UINT64 Bytes = Total * Bs;
    UINT64 Clusters;
    UINT32 Spc;
    UINT32 Reserved = 32;
    UINT32 Tmp;
    UINT32 FatSz;
    UINT32 Vid = 0;
    UINT8 *S;
    UINT8 Lab[11];
    UINTN I;
    UINTN L = StrLen((CHAR16 *)Label);
    CHAR16 C;
    EFI_TIME Tm;
    EFI_STATUS St;

    if (!B->Media->MediaPresent) {
        return EFI_NO_MEDIA;
    }
    if (B->Media->ReadOnly) {
        return EFI_WRITE_PROTECTED;
    }
    if (Bs < 512 || Bs > 4096 || (Bs & (Bs - 1)) != 0 || Total > 0xFFFFFFFFULL || Total < 66600) {
        Print(L"Size or sector size not suitable for FAT32 (minimum about 33 MB)\n");
        return EFI_UNSUPPORTED;
    }

    if (Bytes <= ((UINT64)260 << 20)) {
        Spc = 1;
    } else if (Bytes <= ((UINT64)8 << 30)) {
        Spc = 8;
    } else if (Bytes <= ((UINT64)16 << 30)) {
        Spc = 16;
    } else if (Bytes <= ((UINT64)32 << 30)) {
        Spc = 32;
    } else {
        Spc = 64;
    }
    while ((UINT64)Spc * Bs > 32768 && Spc > 1) {
        Spc >>= 1;
    }

    Tmp = (Bs / 4) * Spc + 2;
    FatSz = (UINT32)((Total - Reserved + Tmp - 1) / Tmp);
    Clusters = (Total - Reserved - 2 * (UINT64)FatSz) / Spc;
    if (Clusters < 65525 || Clusters > 0x0FFFFFF0) {
        Print(L"Size not suitable for FAT32\n");
        return EFI_UNSUPPORTED;
    }

    St = ZeroRange(B, 0, (UINT64)Reserved + 2 * (UINT64)FatSz + Spc, FALSE);
    if (EFI_ERROR(St)) {
        return St;
    }
    S = AllocateZeroPool(Bs);
    if (S == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    if (!EFI_ERROR(uefi_call_wrapper(RT->GetTime, 2, &Tm, NULL))) {
        Vid = ((UINT32)Tm.Hour << 24) | ((UINT32)Tm.Minute << 16) | ((UINT32)Tm.Second << 8) | Tm.Day;
        Vid ^= (UINT32)Tm.Year << 11;
    }

    S[0] = 0xEB;
    S[1] = 0x58;
    S[2] = 0x90;
    CopyMem(S + 3, "IZHOSFS ", 8);
    Put16(S, 11, (UINT16)Bs);
    S[13] = (UINT8)Spc;
    Put16(S, 14, (UINT16)Reserved);
    S[16] = 2;
    S[21] = 0xF8;
    Put16(S, 24, 63);
    Put16(S, 26, 255);
    Put32(S, 28, (UINT32)StartLba);
    Put32(S, 32, (UINT32)Total);
    Put32(S, 36, FatSz);
    Put32(S, 44, 2);
    Put16(S, 48, 1);
    Put16(S, 50, 6);
    S[64] = 0x80;
    S[66] = 0x29;
    Put32(S, 67, Vid);
    for (I = 0; I < 11; I++) {
        C = I < L ? Label[I] : L' ';
        if (C >= L'a' && C <= L'z') {
            C = (CHAR16)(C - 32);
        }
        if (C < 32 || C > 126) {
            C = L'_';
        }
        Lab[I] = (UINT8)C;
        S[71 + I] = Lab[I];
    }
    CopyMem(S + 82, "FAT32   ", 8);
    S[510] = 0x55;
    S[511] = 0xAA;
    St = Wr(B, 0, Bs, S);
    if (!EFI_ERROR(St)) {
        St = Wr(B, 6, Bs, S);
    }

    ZeroMem(S, Bs);
    Put32(S, 0, 0x41615252);
    Put32(S, 484, 0x61417272);
    Put32(S, 488, (UINT32)(Clusters - 1));
    Put32(S, 492, 3);
    Put32(S, 508, 0xAA550000);
    if (!EFI_ERROR(St)) {
        St = Wr(B, 1, Bs, S);
    }
    if (!EFI_ERROR(St)) {
        St = Wr(B, 7, Bs, S);
    }

    ZeroMem(S, Bs);
    Put32(S, 0, 0x0FFFFFF8);
    Put32(S, 4, 0x0FFFFFFF);
    Put32(S, 8, 0x0FFFFFFF);
    if (!EFI_ERROR(St)) {
        St = Wr(B, Reserved, Bs, S);
    }
    if (!EFI_ERROR(St)) {
        St = Wr(B, (UINT64)Reserved + FatSz, Bs, S);
    }

    ZeroMem(S, Bs);
    CopyMem(S, Lab, 11);
    S[11] = 0x08;
    if (!EFI_ERROR(St)) {
        St = Wr(B, (UINT64)Reserved + 2 * (UINT64)FatSz, Bs, S);
    }

    FreePool(S);
    return St;
}

static VOID CmdFrmt(INTN Argc, CHAR16 **Argv)
{
    TargetInfo T;
    EFI_STATUS S;
    CHAR16 Msg[64];

    if (Argc < 2) {
        Print(L"Usage: frmt <disk>[:<part>] [label] [/y]\n");
        return;
    }
    if (!GetTarget(Argv[1], &T)) {
        return;
    }
    if (!T.IsPart && gDisks[T.Disk].Parts > 0) {
        Print(L"Disk %d has partitions; use frmt %d:<part>\n", (INTN)T.Disk, (INTN)T.Disk);
        return;
    }
    if (T.IsPart) {
        SPrint(Msg, sizeof(Msg), L"Format partition %d:%d as FAT32", (INTN)T.Disk, (INTN)T.Num);
    } else {
        SPrint(Msg, sizeof(Msg), L"Format disk %d as FAT32", (INTN)T.Disk);
    }
    if (!Confirm(Msg)) {
        Print(L"Aborted\n");
        return;
    }
    StopFs(T.H);
    S = Fat32(T.Bio, T.Start, Argc > 2 ? Argv[2] : L"NO NAME");
    Flush(T.Bio);
    uefi_call_wrapper(BS->ConnectController, 4, T.H, NULL, NULL, TRUE);
    if (EFI_ERROR(S)) {
        Print(L"frmt failed: %r\n", S);
    } else {
        Print(L"Formatted %s as FAT32\n", Argv[1]);
    }
}

static VOID CmdHelp(VOID)
{
    Print(L"IzhDisk 1.0\n\n");
    Print(L"  listdisk                       list disks\n");
    Print(L"  listpar [disk]                 list partitions\n");
    Print(L"  mount <disk>[:<part>]          connect drivers and mount file systems\n");
    Print(L"  unmount <disk>[:<part>]        disconnect file system drivers\n");
    Print(L"  delete <disk>:<part>           remove a partition (MBR or GPT)\n");
    Print(L"  zero <disk> [/q]               overwrite a disk with zeros (/q: ends only)\n");
    Print(L"  frmt <disk>[:<part>] [label]   format as FAT32\n");
    Print(L"  exit                           leave IzhDisk\n\n");
    Print(L"  /y skips the confirmation prompt\n");
}

static BOOLEAN Process(CHAR16 *Line)
{
    CHAR16 *Argv[MAXARGS];
    CHAR16 *P = Line;
    INTN Argc = 0;
    INTN N = 0;
    INTN I;

    gYes = FALSE;
    gQuick = FALSE;
    while (*P && Argc < MAXARGS) {
        while (*P == L' ' || *P == L'\t') {
            P++;
        }
        if (!*P) {
            break;
        }
        Argv[Argc++] = P;
        while (*P && *P != L' ' && *P != L'\t') {
            P++;
        }
        if (*P) {
            *P++ = 0;
        }
    }
    for (I = 0; I < Argc; I++) {
        if (!StriCmp(Argv[I], L"/y")) {
            gYes = TRUE;
        } else if (!StriCmp(Argv[I], L"/q")) {
            gQuick = TRUE;
        } else {
            Argv[N++] = Argv[I];
        }
    }
    Argc = N;
    if (Argc == 0) {
        return TRUE;
    }

    if (!StriCmp(Argv[0], L"exit") || !StriCmp(Argv[0], L"quit")) {
        return FALSE;
    }
    Scan();
    if (!StriCmp(Argv[0], L"listdisk")) {
        CmdListDisk();
    } else if (!StriCmp(Argv[0], L"listpar")) {
        CmdListPar(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"mount")) {
        CmdMount(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"unmount")) {
        CmdUnmount(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"delete")) {
        CmdDelete(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"zero")) {
        CmdZero(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"frmt")) {
        CmdFrmt(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"help") || !StriCmp(Argv[0], L"?")) {
        CmdHelp();
    } else {
        Print(L"Unknown command '%s'. Type help.\n", Argv[0]);
    }
    return TRUE;
}

static VOID ReadLine(CHAR16 *Buf, UINTN Max)
{
    UINTN Len = 0;
    EFI_INPUT_KEY K;
    CHAR16 One[2];

    Buf[0] = 0;
    for (;;) {
        K = GetKey();
        if (K.UnicodeChar == L'\r' || K.UnicodeChar == L'\n') {
            Print(L"\n");
            return;
        }
        if (K.UnicodeChar == 8) {
            if (Len > 0) {
                Len--;
                Buf[Len] = 0;
                Print(L"\b \b");
            }
            continue;
        }
        if (K.UnicodeChar >= 32 && Len + 1 < Max) {
            Buf[Len++] = K.UnicodeChar;
            Buf[Len] = 0;
            One[0] = K.UnicodeChar;
            One[1] = 0;
            Print(L"%s", One);
        }
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE *Li = NULL;
    CHAR16 Line[MAXLINE];
    CHAR16 *P;

    InitializeLib(ImageHandle, SystemTable);
    ConnectAll();

    if (!EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&Li)) && Li != NULL && Li->LoadOptions != NULL && Li->LoadOptionsSize >= 4) {
        StrnCpy(Line, (CHAR16 *)Li->LoadOptions, MAXLINE - 1);
        Line[MAXLINE - 1] = 0;
        P = Line;
        while (*P == L' ') {
            P++;
        }
        if (*P) {
            Process(P);
            return EFI_SUCCESS;
        }
    }

    Print(L"IzhDisk 1.0 - type help for commands\n");
    for (;;) {
        Print(L"IzhDisk> ");
        ReadLine(Line, MAXLINE);
        if (!Process(Line)) {
            break;
        }
    }
    return EFI_SUCCESS;
}

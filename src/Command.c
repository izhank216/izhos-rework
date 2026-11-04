#include <efi.h>
#include <efilib.h>

#define IZH_OPERATORS 1

static BOOLEAN gFailed;

static BOOLEAN FmtHas(CONST CHAR16 *F, CONST CHAR16 *P)
{
    UINTN I;
    UINTN J;

    for (I = 0; F[I]; I++) {
        for (J = 0; P[J] && F[I + J] == P[J]; J++) {
        }
        if (P[J] == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID NoteFmt(CONST CHAR16 *F)
{
    static CONST CHAR16 *Bad[] = {
        L"%r", L"Usage", L"too long", L"not a dir", L"is a dir", L"not recognized",
        L"cannot", L"must be", L"are the same", L"No matches", L"not empty",
        L"already exists", L"Invalid", L"unavailable", L"Out of memory", L"too small",
        L"Syntax error", NULL
    };
    UINTN I;

    for (I = 0; Bad[I] != NULL; I++) {
        if (FmtHas(F, Bad[I])) {
            gFailed = TRUE;
            return;
        }
    }
}

#define Print(Fmt, ...) (NoteFmt(Fmt), Print(Fmt, ##__VA_ARGS__))

#define MAXP 512
#define MAXLINE 512
#define MAXARGS 32
#define HISTORY 16
#define SYSTEM_DIR L"\\IzhOS\\System"
#define SHELL_VERSION L"1.5"
#define NPOS ((UINTN)-1)

typedef struct {
    CHAR16 **Names;
    BOOLEAN *IsDir;
    UINTN Count;
    UINTN Cap;
} NAMELIST;

typedef struct {
    CHAR8 *Data;
    UINTN Len;
    UINTN Cap;
} EDLINE;

typedef struct {
    EDLINE *Lines;
    UINTN Count;
    UINTN Cap;
    UINTN Cx;
    UINTN Cy;
    UINTN WantX;
    UINTN Top;
    UINTN Left;
    UINTN Cols;
    UINTN Rows;
    BOOLEAN Dirty;
    BOOLEAN CrLf;
    BOOLEAN HasPath;
    BOOLEAN HasClip;
    CHAR16 Path[MAXP];
    CHAR16 Search[128];
    CHAR16 Status[160];
    EDLINE Clip;
    CHAR16 *Row;
    CHAR16 *Tmp;
    UINTN BufCap;
} EDITOR;

static EFI_HANDLE gImage;
static EFI_HANDLE gDevice;
static EFI_FILE_HANDLE gRoot;
static CHAR16 gCwd[MAXP];
static CHAR16 gHistory[HISTORY][MAXLINE];
static UINTN gHistCount;
static BOOLEAN gExit;

static CHAR16 UpCase(CHAR16 C)
{
    if (C >= L'a' && C <= L'z') {
        return (CHAR16)(C - 32);
    }
    return C;
}

static VOID Out(CONST CHAR16 *S)
{
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, (CHAR16 *)S);
}

static VOID ResetConsole(VOID)
{
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, TRUE);
}

static EFI_INPUT_KEY GetKey(VOID)
{
    EFI_INPUT_KEY Key;
    UINTN Index;
    EFI_STATUS Status;

    for (;;) {
        Status = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
        if (!EFI_ERROR(Status)) {
            return Key;
        }
        uefi_call_wrapper(BS->WaitForEvent, 3, (UINTN)1, &ST->ConIn->WaitForKey, &Index);
    }
}

static BOOLEAN IsSep(CHAR16 C)
{
    return C == L'\\' || C == L'/';
}

static BOOLEAN HasWild(CHAR16 *S)
{
    while (*S) {
        if (*S == L'*' || *S == L'?') {
            return TRUE;
        }
        S++;
    }
    return FALSE;
}

static BOOLEAN WildMatch(CHAR16 *Pat, CHAR16 *Str)
{
    CHAR16 *StarP = NULL;
    CHAR16 *StarS = NULL;

    while (*Str) {
        if (*Pat == L'*') {
            StarP = Pat++;
            StarS = Str;
        } else if (*Pat == L'?' || UpCase(*Pat) == UpCase(*Str)) {
            Pat++;
            Str++;
        } else if (StarP != NULL) {
            Pat = StarP + 1;
            Str = ++StarS;
        } else {
            return FALSE;
        }
    }
    while (*Pat == L'*') {
        Pat++;
    }
    return *Pat == 0;
}

static BOOLEAN ContainsNoCase(CHAR16 *Hay, CHAR16 *Needle)
{
    UINTN H = StrLen(Hay);
    UINTN N = StrLen(Needle);
    UINTN I;
    UINTN J;

    if (N == 0) {
        return TRUE;
    }
    if (N > H) {
        return FALSE;
    }
    for (I = 0; I + N <= H; I++) {
        for (J = 0; J < N; J++) {
            if (UpCase(Hay[I + J]) != UpCase(Needle[J])) {
                break;
            }
        }
        if (J == N) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN InsideOrEqual(CHAR16 *Path, CHAR16 *Dir)
{
    UINTN Len = StrLen(Dir);
    UINTN I;

    if (Len == 1 && Dir[0] == L'\\') {
        return TRUE;
    }
    if (StrLen(Path) < Len) {
        return FALSE;
    }
    for (I = 0; I < Len; I++) {
        if (UpCase(Path[I]) != UpCase(Dir[I])) {
            return FALSE;
        }
    }
    return Path[Len] == 0 || Path[Len] == L'\\';
}

static BOOLEAN Resolve(CHAR16 *Input, CHAR16 *Result)
{
    CHAR16 Joined[MAXP];
    CHAR16 Comp[MAXP];
    UINTN I = 0;
    UINTN N;
    UINTN Len;

    if (StrLen(Input) == 0) {
        StrCpy(Result, gCwd);
        return TRUE;
    }

    if (IsSep(Input[0])) {
        if (StrLen(Input) >= MAXP) {
            return FALSE;
        }
        StrCpy(Joined, Input);
    } else {
        if (StrLen(gCwd) + StrLen(Input) + 2 >= MAXP) {
            return FALSE;
        }
        StrCpy(Joined, gCwd);
        StrCat(Joined, L"\\");
        StrCat(Joined, Input);
    }

    Result[0] = 0;
    while (Joined[I]) {
        while (IsSep(Joined[I])) {
            I++;
        }
        if (!Joined[I]) {
            break;
        }
        N = 0;
        while (Joined[I] && !IsSep(Joined[I])) {
            Comp[N++] = Joined[I++];
        }
        Comp[N] = 0;

        if (!StrCmp(Comp, L".")) {
            continue;
        }
        if (!StrCmp(Comp, L"..")) {
            Len = StrLen(Result);
            while (Len > 0 && Result[Len - 1] != L'\\') {
                Len--;
            }
            if (Len > 0) {
                Result[Len - 1] = 0;
            }
            continue;
        }
        if (StrLen(Result) + N + 2 >= MAXP) {
            return FALSE;
        }
        StrCat(Result, L"\\");
        StrCat(Result, Comp);
    }

    if (Result[0] == 0) {
        StrCpy(Result, L"\\");
    }
    return TRUE;
}

static BOOLEAN JoinPath(CHAR16 *Dir, CHAR16 *Name, CHAR16 *Result)
{
    UINTN D = StrLen(Dir);

    if (D + StrLen(Name) + 2 >= MAXP) {
        return FALSE;
    }
    StrCpy(Result, Dir);
    if (!(D == 1 && Dir[0] == L'\\')) {
        StrCat(Result, L"\\");
    }
    StrCat(Result, Name);
    return TRUE;
}

static CHAR16 *BaseName(CHAR16 *Path)
{
    UINTN I = StrLen(Path);

    while (I > 0 && Path[I - 1] != L'\\') {
        I--;
    }
    return Path + I;
}

static VOID SplitPath(CHAR16 *Abs, CHAR16 *Parent, CHAR16 *Name)
{
    UINTN I = StrLen(Abs);

    while (I > 0 && Abs[I - 1] != L'\\') {
        I--;
    }
    StrCpy(Name, Abs + I);
    if (I <= 1) {
        StrCpy(Parent, L"\\");
    } else {
        CopyMem(Parent, Abs, (I - 1) * sizeof(CHAR16));
        Parent[I - 1] = 0;
    }
}

static EFI_STATUS OpenAbs(CHAR16 *Path, UINT64 Mode, UINT64 Attr, EFI_FILE_HANDLE *Handle)
{
    return uefi_call_wrapper(gRoot->Open, 5, gRoot, Handle, Path, Mode, Attr);
}

static VOID CloseFile(EFI_FILE_HANDLE File)
{
    uefi_call_wrapper(File->Close, 1, File);
}

static BOOLEAN HandleIsDir(EFI_FILE_HANDLE File)
{
    EFI_FILE_INFO *Info = LibFileInfo(File);
    BOOLEAN Result = FALSE;

    if (Info != NULL) {
        Result = (Info->Attribute & EFI_FILE_DIRECTORY) != 0;
        FreePool(Info);
    }
    return Result;
}

static BOOLEAN Exists(CHAR16 *Path, BOOLEAN *IsDir)
{
    EFI_FILE_HANDLE File;

    if (EFI_ERROR(OpenAbs(Path, EFI_FILE_MODE_READ, 0, &File))) {
        return FALSE;
    }
    if (IsDir != NULL) {
        *IsDir = HandleIsDir(File);
    }
    CloseFile(File);
    return TRUE;
}

static EFI_STATUS DeletePath(CHAR16 *Path)
{
    EFI_FILE_HANDLE File;
    EFI_STATUS Status;

    Status = OpenAbs(Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &File);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Status = uefi_call_wrapper(File->Delete, 1, File);
    if (Status != EFI_SUCCESS) {
        return EFI_ACCESS_DENIED;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS MakeDir(CHAR16 *Path)
{
    EFI_FILE_HANDLE File;
    EFI_STATUS Status;

    Status = OpenAbs(Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY, &File);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    if (!HandleIsDir(File)) {
        CloseFile(File);
        return EFI_ACCESS_DENIED;
    }
    CloseFile(File);
    return EFI_SUCCESS;
}

static EFI_STATUS ReadWhole(CHAR16 *Path, UINT8 **Data, UINTN *Size)
{
    EFI_FILE_HANDLE File;
    EFI_FILE_INFO *Info;
    UINT8 *Buf;
    UINTN Len;
    UINTN Got = 0;
    EFI_STATUS Status;

    Status = OpenAbs(Path, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Info = LibFileInfo(File);
    if (Info == NULL) {
        CloseFile(File);
        return EFI_DEVICE_ERROR;
    }
    if (Info->Attribute & EFI_FILE_DIRECTORY) {
        FreePool(Info);
        CloseFile(File);
        return EFI_ACCESS_DENIED;
    }
    Len = (UINTN)Info->FileSize;
    FreePool(Info);

    Buf = AllocatePool(Len + 1);
    if (Buf == NULL) {
        CloseFile(File);
        return EFI_OUT_OF_RESOURCES;
    }
    if (Len > 0) {
        Got = Len;
        Status = uefi_call_wrapper(File->Read, 3, File, &Got, Buf);
        if (EFI_ERROR(Status)) {
            FreePool(Buf);
            CloseFile(File);
            return Status;
        }
    }
    Buf[Got] = 0;
    *Data = Buf;
    *Size = Got;
    CloseFile(File);
    return EFI_SUCCESS;
}

static BOOLEAN ListAdd(NAMELIST *L, CHAR16 *Name, BOOLEAN IsDir)
{
    CHAR16 **NN;
    BOOLEAN *ND;
    UINTN NC;
    UINTN I;

    if (L->Count == L->Cap) {
        NC = L->Cap ? L->Cap * 2 : 32;
        NN = AllocatePool(NC * sizeof(CHAR16 *));
        ND = AllocatePool(NC * sizeof(BOOLEAN));
        if (NN == NULL || ND == NULL) {
            if (NN) {
                FreePool(NN);
            }
            if (ND) {
                FreePool(ND);
            }
            return FALSE;
        }
        for (I = 0; I < L->Count; I++) {
            NN[I] = L->Names[I];
            ND[I] = L->IsDir[I];
        }
        if (L->Names) {
            FreePool(L->Names);
        }
        if (L->IsDir) {
            FreePool(L->IsDir);
        }
        L->Names = NN;
        L->IsDir = ND;
        L->Cap = NC;
    }
    L->Names[L->Count] = StrDuplicate(Name);
    if (L->Names[L->Count] == NULL) {
        return FALSE;
    }
    L->IsDir[L->Count] = IsDir;
    L->Count++;
    return TRUE;
}

static VOID ListFree(NAMELIST *L)
{
    UINTN I;

    for (I = 0; I < L->Count; I++) {
        FreePool(L->Names[I]);
    }
    if (L->Names) {
        FreePool(L->Names);
    }
    if (L->IsDir) {
        FreePool(L->IsDir);
    }
    ZeroMem(L, sizeof(*L));
}

static EFI_STATUS ReadDir(EFI_FILE_HANDLE Dir, NAMELIST *L, CHAR16 *Pattern)
{
    UINT64 Buf[256];
    UINTN Size;
    EFI_FILE_INFO *Info;
    EFI_STATUS Status;

    uefi_call_wrapper(Dir->SetPosition, 2, Dir, (UINT64)0);
    for (;;) {
        Size = sizeof(Buf);
        Status = uefi_call_wrapper(Dir->Read, 3, Dir, &Size, Buf);
        if (EFI_ERROR(Status)) {
            return Status;
        }
        if (Size == 0) {
            break;
        }
        Info = (EFI_FILE_INFO *)Buf;
        if (!StrCmp(Info->FileName, L".") || !StrCmp(Info->FileName, L"..")) {
            continue;
        }
        if (Pattern != NULL && !WildMatch(Pattern, Info->FileName)) {
            continue;
        }
        if (!ListAdd(L, Info->FileName, (Info->Attribute & EFI_FILE_DIRECTORY) != 0)) {
            return EFI_OUT_OF_RESOURCES;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS Gather(CHAR16 *Arg, CHAR16 *Abs, CHAR16 *Parent, NAMELIST *L, BOOLEAN *Wild)
{
    CHAR16 Name[MAXP];
    EFI_FILE_HANDLE Dir;
    EFI_STATUS Status;

    *Wild = FALSE;
    if (!Resolve(Arg, Abs)) {
        return EFI_BAD_BUFFER_SIZE;
    }
    SplitPath(Abs, Parent, Name);
    if (!HasWild(Name)) {
        return EFI_SUCCESS;
    }
    *Wild = TRUE;
    Status = OpenAbs(Parent, EFI_FILE_MODE_READ, 0, &Dir);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Status = ReadDir(Dir, L, Name);
    CloseFile(Dir);
    return Status;
}

static EFI_STATUS DeleteTree(CHAR16 *Abs)
{
    EFI_FILE_HANDLE Dir;
    NAMELIST L;
    UINTN I;
    CHAR16 Child[MAXP];
    EFI_STATUS Status;

    ZeroMem(&L, sizeof(L));
    Status = OpenAbs(Abs, EFI_FILE_MODE_READ, 0, &Dir);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Status = ReadDir(Dir, &L, NULL);
    CloseFile(Dir);

    if (!EFI_ERROR(Status)) {
        for (I = 0; I < L.Count; I++) {
            if (!JoinPath(Abs, L.Names[I], Child)) {
                Status = EFI_BAD_BUFFER_SIZE;
                break;
            }
            Status = L.IsDir[I] ? DeleteTree(Child) : DeletePath(Child);
            if (EFI_ERROR(Status)) {
                break;
            }
        }
    }
    ListFree(&L);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    return DeletePath(Abs);
}

static EFI_STATUS CopyFileAbs(CHAR16 *Src, CHAR16 *Dst)
{
    EFI_FILE_HANDLE Source;
    EFI_FILE_HANDLE Dest;
    EFI_STATUS Status;
    UINT8 *Buf;
    UINTN Size;
    UINTN Wrote;

    Status = OpenAbs(Src, EFI_FILE_MODE_READ, 0, &Source);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    if (HandleIsDir(Source)) {
        CloseFile(Source);
        return EFI_ACCESS_DENIED;
    }

    if (!EFI_ERROR(OpenAbs(Dst, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &Dest))) {
        uefi_call_wrapper(Dest->Delete, 1, Dest);
    }
    Status = OpenAbs(Dst, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &Dest);
    if (EFI_ERROR(Status)) {
        CloseFile(Source);
        return Status;
    }

    Buf = AllocatePool(65536);
    if (Buf == NULL) {
        CloseFile(Source);
        CloseFile(Dest);
        return EFI_OUT_OF_RESOURCES;
    }

    for (;;) {
        Size = 65536;
        Status = uefi_call_wrapper(Source->Read, 3, Source, &Size, Buf);
        if (EFI_ERROR(Status)) {
            break;
        }
        if (Size == 0) {
            Status = EFI_SUCCESS;
            break;
        }
        Wrote = Size;
        Status = uefi_call_wrapper(Dest->Write, 3, Dest, &Wrote, Buf);
        if (EFI_ERROR(Status)) {
            break;
        }
        if (Wrote != Size) {
            Status = EFI_VOLUME_FULL;
            break;
        }
    }

    FreePool(Buf);
    CloseFile(Source);
    CloseFile(Dest);
    return Status;
}

static EFI_STATUS CopyAny(CHAR16 *Src, CHAR16 *Dst);

static EFI_STATUS CopyTree(CHAR16 *Src, CHAR16 *Dst)
{
    EFI_FILE_HANDLE Dir;
    NAMELIST L;
    UINTN I;
    CHAR16 From[MAXP];
    CHAR16 To[MAXP];
    EFI_STATUS Status;

    if (InsideOrEqual(Dst, Src)) {
        return EFI_INVALID_PARAMETER;
    }
    Status = MakeDir(Dst);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    ZeroMem(&L, sizeof(L));
    Status = OpenAbs(Src, EFI_FILE_MODE_READ, 0, &Dir);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Status = ReadDir(Dir, &L, NULL);
    CloseFile(Dir);

    if (!EFI_ERROR(Status)) {
        for (I = 0; I < L.Count; I++) {
            if (!JoinPath(Src, L.Names[I], From) || !JoinPath(Dst, L.Names[I], To)) {
                Status = EFI_BAD_BUFFER_SIZE;
                break;
            }
            Status = CopyAny(From, To);
            if (EFI_ERROR(Status)) {
                break;
            }
        }
    }
    ListFree(&L);
    return Status;
}

static EFI_STATUS CopyAny(CHAR16 *Src, CHAR16 *Dst)
{
    EFI_FILE_HANDLE File;
    EFI_STATUS Status;
    BOOLEAN IsDir;

    Status = OpenAbs(Src, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    IsDir = HandleIsDir(File);
    CloseFile(File);

    if (IsDir) {
        return CopyTree(Src, Dst);
    }
    return CopyFileAbs(Src, Dst);
}

static EFI_STATUS MoveOne(CHAR16 *From, CHAR16 *To)
{
    EFI_FILE_HANDLE File;
    EFI_FILE_INFO *Old;
    EFI_FILE_INFO *New;
    UINTN Size;
    EFI_STATUS Status;
    BOOLEAN IsDir;

    if (!StriCmp(From, To)) {
        return EFI_SUCCESS;
    }
    if (InsideOrEqual(To, From)) {
        return EFI_INVALID_PARAMETER;
    }

    Status = OpenAbs(From, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &File);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Old = LibFileInfo(File);
    if (Old == NULL) {
        CloseFile(File);
        return EFI_DEVICE_ERROR;
    }
    IsDir = (Old->Attribute & EFI_FILE_DIRECTORY) != 0;

    Size = SIZE_OF_EFI_FILE_INFO + StrSize(To);
    New = AllocateZeroPool(Size);
    if (New != NULL) {
        CopyMem(New, Old, SIZE_OF_EFI_FILE_INFO);
        StrCpy(New->FileName, To);
        New->Size = Size;
        Status = uefi_call_wrapper(File->SetInfo, 4, File, &GenericFileInfo, Size, New);
        FreePool(New);
    } else {
        Status = EFI_OUT_OF_RESOURCES;
    }
    FreePool(Old);
    CloseFile(File);

    if (!EFI_ERROR(Status)) {
        return EFI_SUCCESS;
    }

    Status = CopyAny(From, To);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    return IsDir ? DeleteTree(From) : DeletePath(From);
}

static VOID PrintEntry(EFI_FILE_INFO *Info)
{
    EFI_TIME *T = &Info->ModificationTime;

    if (Info->Attribute & EFI_FILE_DIRECTORY) {
        Print(L"%02d/%02d/%04d  %02d:%02d  %s  %s\n", (INTN)T->Day, (INTN)T->Month, (INTN)T->Year, (INTN)T->Hour, (INTN)T->Minute, L"<DIR>       ", Info->FileName);
    } else {
        Print(L"%02d/%02d/%04d  %02d:%02d  %12lu  %s\n", (INTN)T->Day, (INTN)T->Month, (INTN)T->Year, (INTN)T->Hour, (INTN)T->Minute, (UINT64)Info->FileSize, Info->FileName);
    }
}

static VOID CmdDir(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    CHAR16 Parent[MAXP];
    CHAR16 Name[MAXP];
    CHAR16 DirPath[MAXP];
    CHAR16 *Pattern = NULL;
    EFI_FILE_HANDLE Dir;
    EFI_STATUS Status;
    UINT64 Buf[256];
    UINTN Size;
    EFI_FILE_INFO *Info;
    UINTN Files = 0;
    UINTN Dirs = 0;
    UINT64 Bytes = 0;

    if (!Resolve(Argc > 1 ? Argv[1] : L"", Abs)) {
        Print(L"Path too long\n");
        return;
    }
    if (HasWild(Abs)) {
        SplitPath(Abs, Parent, Name);
        StrCpy(DirPath, Parent);
        Pattern = Name;
    } else {
        StrCpy(DirPath, Abs);
    }

    Status = OpenAbs(DirPath, EFI_FILE_MODE_READ, 0, &Dir);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", DirPath, Status);
        return;
    }

    if (!HandleIsDir(Dir)) {
        Info = LibFileInfo(Dir);
        if (Info != NULL) {
            PrintEntry(Info);
            FreePool(Info);
        }
        CloseFile(Dir);
        return;
    }

    Print(L"\n Directory of %s\n\n", DirPath);
    uefi_call_wrapper(Dir->SetPosition, 2, Dir, (UINT64)0);
    for (;;) {
        Size = sizeof(Buf);
        Status = uefi_call_wrapper(Dir->Read, 3, Dir, &Size, Buf);
        if (EFI_ERROR(Status) || Size == 0) {
            break;
        }
        Info = (EFI_FILE_INFO *)Buf;
        if (Pattern != NULL && !WildMatch(Pattern, Info->FileName)) {
            continue;
        }
        PrintEntry(Info);
        if (Info->Attribute & EFI_FILE_DIRECTORY) {
            Dirs++;
        } else {
            Files++;
            Bytes += Info->FileSize;
        }
    }
    CloseFile(Dir);
    Print(L"\n%12lu File(s)  %lu bytes\n%12lu Dir(s)\n\n", (UINT64)Files, Bytes, (UINT64)Dirs);
}

static VOID CmdCd(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    BOOLEAN IsDir;

    if (Argc < 2) {
        Print(L"%s\n", gCwd);
        return;
    }
    if (!Resolve(Argv[1], Abs)) {
        Print(L"Path too long\n");
        return;
    }
    if (!Exists(Abs, &IsDir)) {
        Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
        return;
    }
    if (!IsDir) {
        Print(L"%s: not a directory\n", Argv[1]);
        return;
    }
    StrCpy(gCwd, Abs);
}

static VOID CmdDel(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    CHAR16 Parent[MAXP];
    CHAR16 Path[MAXP];
    NAMELIST L;
    BOOLEAN Wild;
    BOOLEAN IsDir;
    UINTN Done = 0;
    UINTN I;
    INTN A;
    EFI_STATUS Status;

    if (Argc < 2) {
        Print(L"Usage: del <file|pattern> ...\n");
        return;
    }

    for (A = 1; A < Argc; A++) {
        if (Argv[A][0] == L'/' || Argv[A][0] == L'-') {
            continue;
        }
        ZeroMem(&L, sizeof(L));
        Status = Gather(Argv[A], Abs, Parent, &L, &Wild);
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[A], Status);
            ListFree(&L);
            continue;
        }
        if (Wild) {
            for (I = 0; I < L.Count; I++) {
                if (L.IsDir[I]) {
                    continue;
                }
                if (!JoinPath(Parent, L.Names[I], Path)) {
                    continue;
                }
                Status = DeletePath(Path);
                if (EFI_ERROR(Status)) {
                    Print(L"%s: %r\n", Path, Status);
                } else {
                    Done++;
                }
            }
        } else {
            if (!Exists(Abs, &IsDir)) {
                Print(L"%s: %r\n", Argv[A], EFI_NOT_FOUND);
            } else if (IsDir) {
                Print(L"%s: is a directory, use rmdir\n", Argv[A]);
            } else {
                Status = DeletePath(Abs);
                if (EFI_ERROR(Status)) {
                    Print(L"%s: %r\n", Argv[A], Status);
                } else {
                    Done++;
                }
            }
        }
        ListFree(&L);
    }
    Print(L"%d file(s) deleted\n", (INTN)Done);
}

static VOID CmdRmdir(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    BOOLEAN Recursive = FALSE;
    BOOLEAN IsDir;
    BOOLEAN Any = FALSE;
    EFI_FILE_HANDLE Dir;
    NAMELIST L;
    INTN A;
    EFI_STATUS Status;

    for (A = 1; A < Argc; A++) {
        if (!StriCmp(Argv[A], L"/s") || !StriCmp(Argv[A], L"-s") || !StriCmp(Argv[A], L"-r") || !StriCmp(Argv[A], L"/r")) {
            Recursive = TRUE;
        }
    }

    for (A = 1; A < Argc; A++) {
        if (Argv[A][0] == L'/' || Argv[A][0] == L'-') {
            continue;
        }
        Any = TRUE;
        if (!Resolve(Argv[A], Abs)) {
            Print(L"Path too long\n");
            continue;
        }
        if (!StrCmp(Abs, L"\\")) {
            Print(L"%s: cannot remove the root directory\n", Argv[A]);
            continue;
        }
        if (InsideOrEqual(gCwd, Abs)) {
            Print(L"%s: cannot remove the current directory\n", Argv[A]);
            continue;
        }
        if (!Exists(Abs, &IsDir)) {
            Print(L"%s: %r\n", Argv[A], EFI_NOT_FOUND);
            continue;
        }
        if (!IsDir) {
            Print(L"%s: not a directory\n", Argv[A]);
            continue;
        }
        if (Recursive) {
            Status = DeleteTree(Abs);
        } else {
            ZeroMem(&L, sizeof(L));
            Status = OpenAbs(Abs, EFI_FILE_MODE_READ, 0, &Dir);
            if (!EFI_ERROR(Status)) {
                Status = ReadDir(Dir, &L, NULL);
                CloseFile(Dir);
                if (!EFI_ERROR(Status) && L.Count > 0) {
                    Status = EFI_ACCESS_DENIED;
                    Print(L"%s: directory not empty (use rmdir /s)\n", Argv[A]);
                    ListFree(&L);
                    continue;
                }
            }
            ListFree(&L);
            if (!EFI_ERROR(Status)) {
                Status = DeletePath(Abs);
            }
        }
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[A], Status);
        }
    }
    if (!Any) {
        Print(L"Usage: rmdir [/s] <directory> ...\n");
    }
}

static VOID CmdMkdir(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    INTN A;
    EFI_STATUS Status;

    if (Argc < 2) {
        Print(L"Usage: mkdir <directory> ...\n");
        return;
    }
    for (A = 1; A < Argc; A++) {
        if (!Resolve(Argv[A], Abs)) {
            Print(L"Path too long\n");
            continue;
        }
        Status = MakeDir(Abs);
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[A], Status);
        }
    }
}

static VOID CmdType(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    CHAR16 Text[4200];
    UINT8 Chunk[1024];
    EFI_FILE_HANDLE File;
    EFI_STATUS Status;
    UINTN Size;
    UINTN I;
    UINTN N;
    UINT8 C;

    if (Argc < 2) {
        Print(L"Usage: type <file>\n");
        return;
    }
    if (!Resolve(Argv[1], Abs)) {
        Print(L"Path too long\n");
        return;
    }
    Status = OpenAbs(Abs, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Argv[1], Status);
        return;
    }
    if (HandleIsDir(File)) {
        CloseFile(File);
        Print(L"%s: is a directory\n", Argv[1]);
        return;
    }

    for (;;) {
        Size = sizeof(Chunk);
        Status = uefi_call_wrapper(File->Read, 3, File, &Size, Chunk);
        if (EFI_ERROR(Status) || Size == 0) {
            break;
        }
        N = 0;
        for (I = 0; I < Size; I++) {
            C = Chunk[I];
            if (C == '\r') {
                continue;
            }
            if (C == '\n') {
                Text[N++] = L'\r';
                Text[N++] = L'\n';
            } else if (C == '\t') {
                Text[N++] = L' ';
                Text[N++] = L' ';
                Text[N++] = L' ';
                Text[N++] = L' ';
            } else if (C >= 32 && C < 127) {
                Text[N++] = (CHAR16)C;
            } else if (C >= 128) {
                Text[N++] = L'?';
            }
        }
        Text[N] = 0;
        Out(Text);
    }
    CloseFile(File);
    Out(L"\r\n");
}

static UINTN FindNames(CHAR16 *Dir, CHAR16 *Pat, BOOLEAN Wild)
{
    EFI_FILE_HANDLE Handle;
    NAMELIST L;
    UINTN I;
    UINTN Found = 0;
    CHAR16 Child[MAXP];
    BOOLEAN Match;

    ZeroMem(&L, sizeof(L));
    if (EFI_ERROR(OpenAbs(Dir, EFI_FILE_MODE_READ, 0, &Handle))) {
        return 0;
    }
    ReadDir(Handle, &L, NULL);
    CloseFile(Handle);

    for (I = 0; I < L.Count; I++) {
        if (!JoinPath(Dir, L.Names[I], Child)) {
            continue;
        }
        Match = Wild ? WildMatch(Pat, L.Names[I]) : ContainsNoCase(L.Names[I], Pat);
        if (Match) {
            Print(L"%s%s\n", Child, L.IsDir[I] ? L"\\" : L"");
            Found++;
        }
        if (L.IsDir[I]) {
            Found += FindNames(Child, Pat, Wild);
        }
    }
    ListFree(&L);
    return Found;
}

static VOID FindText(CHAR16 *Arg, CHAR16 *Needle)
{
    CHAR16 Abs[MAXP];
    CHAR16 Line[MAXLINE];
    UINT8 *Data;
    UINTN Size;
    UINTN I;
    UINTN N = 0;
    UINTN LineNo = 1;
    EFI_STATUS Status;

    if (!Resolve(Arg, Abs)) {
        Print(L"Path too long\n");
        return;
    }
    Status = ReadWhole(Abs, &Data, &Size);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Arg, Status);
        return;
    }

    for (I = 0; I <= Size; I++) {
        if (I == Size || Data[I] == '\n') {
            Line[N] = 0;
            if (ContainsNoCase(Line, Needle)) {
                Print(L"%s:%d: %s\n", Abs, (INTN)LineNo, Line);
            }
            N = 0;
            LineNo++;
            continue;
        }
        if (Data[I] == '\r') {
            continue;
        }
        if (N < MAXLINE - 1) {
            if (Data[I] >= 32 && Data[I] < 127) {
                Line[N++] = (CHAR16)Data[I];
            } else if (Data[I] == '\t') {
                Line[N++] = L' ';
            } else {
                Line[N++] = L'?';
            }
        }
    }
    FreePool(Data);
}

static VOID CmdFind(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Start[MAXP];
    UINTN Found;
    INTN A;

    if (Argc < 2) {
        Print(L"Usage: find <name|pattern> [directory]\n");
        Print(L"       find /t <text> <file> ...\n");
        return;
    }

    if (!StriCmp(Argv[1], L"/t")) {
        if (Argc < 4) {
            Print(L"Usage: find /t <text> <file> ...\n");
            return;
        }
        for (A = 3; A < Argc; A++) {
            FindText(Argv[A], Argv[2]);
        }
        return;
    }

    if (!Resolve(Argc > 2 ? Argv[2] : L"", Start)) {
        Print(L"Path too long\n");
        return;
    }
    Found = FindNames(Start, Argv[1], HasWild(Argv[1]));
    if (Found == 0) {
        Print(L"No matches\n");
    }
}

static VOID CmdCopy(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Src[MAXP];
    CHAR16 Parent[MAXP];
    CHAR16 Dst[MAXP];
    CHAR16 From[MAXP];
    CHAR16 To[MAXP];
    NAMELIST L;
    BOOLEAN Wild;
    BOOLEAN DstIsDir = FALSE;
    BOOLEAN DstExists;
    UINTN I;
    UINTN Done = 0;
    EFI_STATUS Status;

    if (Argc < 3) {
        Print(L"Usage: copy <source> <destination>\n");
        return;
    }

    ZeroMem(&L, sizeof(L));
    Status = Gather(Argv[1], Src, Parent, &L, &Wild);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Argv[1], Status);
        ListFree(&L);
        return;
    }
    if (!Resolve(Argv[2], Dst)) {
        Print(L"Path too long\n");
        ListFree(&L);
        return;
    }
    DstExists = Exists(Dst, &DstIsDir);

    if (Wild) {
        if (L.Count == 0) {
            Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
            ListFree(&L);
            return;
        }
        if (!DstExists || !DstIsDir) {
            Print(L"Destination must be an existing directory\n");
            ListFree(&L);
            return;
        }
        for (I = 0; I < L.Count; I++) {
            if (!JoinPath(Parent, L.Names[I], From) || !JoinPath(Dst, L.Names[I], To)) {
                continue;
            }
            Status = CopyAny(From, To);
            if (EFI_ERROR(Status)) {
                Print(L"%s: %r\n", From, Status);
                break;
            }
            Done++;
        }
    } else {
        if (!Exists(Src, NULL)) {
            Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
            ListFree(&L);
            return;
        }
        if (DstExists && DstIsDir) {
            if (!JoinPath(Dst, BaseName(Src), To)) {
                Print(L"Path too long\n");
                ListFree(&L);
                return;
            }
        } else {
            StrCpy(To, Dst);
        }
        if (!StriCmp(Src, To)) {
            Print(L"Source and destination are the same\n");
            ListFree(&L);
            return;
        }
        Status = CopyAny(Src, To);
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[1], Status);
        } else {
            Done++;
        }
    }
    Print(L"%d item(s) copied\n", (INTN)Done);
    ListFree(&L);
}

static VOID CmdMove(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Src[MAXP];
    CHAR16 Parent[MAXP];
    CHAR16 Dst[MAXP];
    CHAR16 From[MAXP];
    CHAR16 To[MAXP];
    NAMELIST L;
    BOOLEAN Wild;
    BOOLEAN DstIsDir = FALSE;
    BOOLEAN DstExists;
    UINTN I;
    UINTN Done = 0;
    EFI_STATUS Status;

    if (Argc < 3) {
        Print(L"Usage: move <source> <destination>\n");
        return;
    }

    ZeroMem(&L, sizeof(L));
    Status = Gather(Argv[1], Src, Parent, &L, &Wild);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Argv[1], Status);
        ListFree(&L);
        return;
    }
    if (!Resolve(Argv[2], Dst)) {
        Print(L"Path too long\n");
        ListFree(&L);
        return;
    }
    DstExists = Exists(Dst, &DstIsDir);

    if (Wild) {
        if (L.Count == 0) {
            Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
            ListFree(&L);
            return;
        }
        if (!DstExists || !DstIsDir) {
            Print(L"Destination must be an existing directory\n");
            ListFree(&L);
            return;
        }
        for (I = 0; I < L.Count; I++) {
            if (!JoinPath(Parent, L.Names[I], From) || !JoinPath(Dst, L.Names[I], To)) {
                continue;
            }
            Status = MoveOne(From, To);
            if (EFI_ERROR(Status)) {
                Print(L"%s: %r\n", From, Status);
                break;
            }
            Done++;
        }
    } else {
        if (!Exists(Src, NULL)) {
            Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
            ListFree(&L);
            return;
        }
        if (InsideOrEqual(gCwd, Src)) {
            Print(L"%s: cannot move the current directory\n", Argv[1]);
            ListFree(&L);
            return;
        }
        if (DstExists && DstIsDir) {
            if (!JoinPath(Dst, BaseName(Src), To)) {
                Print(L"Path too long\n");
                ListFree(&L);
                return;
            }
        } else {
            StrCpy(To, Dst);
        }
        Status = MoveOne(Src, To);
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[1], Status);
        } else {
            Done++;
        }
    }
    Print(L"%d item(s) moved\n", (INTN)Done);
    ListFree(&L);
}

static BOOLEAN LineReserve(EDLINE *L, UINTN Need)
{
    CHAR8 *N;
    UINTN Cap;

    if (Need <= L->Cap) {
        return TRUE;
    }
    Cap = L->Cap ? L->Cap : 16;
    while (Cap < Need) {
        Cap *= 2;
    }
    N = AllocatePool(Cap);
    if (N == NULL) {
        return FALSE;
    }
    if (L->Data != NULL) {
        CopyMem(N, L->Data, L->Len);
        FreePool(L->Data);
    }
    L->Data = N;
    L->Cap = Cap;
    return TRUE;
}

static VOID MoveBytes(CHAR8 *Dst, CHAR8 *Src, UINTN N)
{
    UINTN I;

    if (Dst < Src) {
        for (I = 0; I < N; I++) {
            Dst[I] = Src[I];
        }
    } else {
        for (I = N; I > 0; I--) {
            Dst[I - 1] = Src[I - 1];
        }
    }
}

static BOOLEAN EdReserve(EDITOR *E, UINTN Need)
{
    EDLINE *N;
    UINTN Cap;
    UINTN I;

    if (Need <= E->Cap) {
        return TRUE;
    }
    Cap = E->Cap ? E->Cap : 64;
    while (Cap < Need) {
        Cap *= 2;
    }
    N = AllocatePool(Cap * sizeof(EDLINE));
    if (N == NULL) {
        return FALSE;
    }
    for (I = 0; I < E->Count; I++) {
        N[I] = E->Lines[I];
    }
    if (E->Lines != NULL) {
        FreePool(E->Lines);
    }
    E->Lines = N;
    E->Cap = Cap;
    return TRUE;
}

static BOOLEAN EdInsertLine(EDITOR *E, UINTN At, CHAR8 *Data, UINTN Len)
{
    EDLINE New;
    UINTN I;

    if (!EdReserve(E, E->Count + 1)) {
        return FALSE;
    }
    New.Data = NULL;
    New.Len = 0;
    New.Cap = 0;
    if (!LineReserve(&New, Len ? Len : 1)) {
        return FALSE;
    }
    for (I = 0; I < Len; I++) {
        New.Data[I] = Data[I];
    }
    New.Len = Len;
    for (I = E->Count; I > At; I--) {
        E->Lines[I] = E->Lines[I - 1];
    }
    E->Lines[At] = New;
    E->Count++;
    return TRUE;
}

static VOID EdDeleteLine(EDITOR *E, UINTN At)
{
    UINTN I;

    if (E->Lines[At].Data != NULL) {
        FreePool(E->Lines[At].Data);
    }
    for (I = At; I + 1 < E->Count; I++) {
        E->Lines[I] = E->Lines[I + 1];
    }
    E->Count--;
    if (E->Count == 0) {
        EdInsertLine(E, 0, NULL, 0);
    }
}

static VOID EdInsertChar(EDITOR *E, CHAR8 C)
{
    EDLINE *L = &E->Lines[E->Cy];

    if (!LineReserve(L, L->Len + 1)) {
        return;
    }
    MoveBytes(L->Data + E->Cx + 1, L->Data + E->Cx, L->Len - E->Cx);
    L->Data[E->Cx] = C;
    L->Len++;
    E->Cx++;
    E->Dirty = TRUE;
}

static VOID EdNewline(EDITOR *E)
{
    EDLINE *L = &E->Lines[E->Cy];
    CHAR8 *Tail = L->Data + E->Cx;
    UINTN TailLen = L->Len - E->Cx;

    if (!EdInsertLine(E, E->Cy + 1, Tail, TailLen)) {
        return;
    }
    E->Lines[E->Cy].Len = E->Cx;
    E->Cy++;
    E->Cx = 0;
    E->Dirty = TRUE;
}

static VOID EdBackspace(EDITOR *E)
{
    EDLINE *L = &E->Lines[E->Cy];
    EDLINE *P;
    UINTN I;

    if (E->Cx > 0) {
        MoveBytes(L->Data + E->Cx - 1, L->Data + E->Cx, L->Len - E->Cx);
        L->Len--;
        E->Cx--;
        E->Dirty = TRUE;
        return;
    }
    if (E->Cy == 0) {
        return;
    }
    P = &E->Lines[E->Cy - 1];
    if (!LineReserve(P, P->Len + L->Len + 1)) {
        return;
    }
    for (I = 0; I < L->Len; I++) {
        P->Data[P->Len + I] = L->Data[I];
    }
    E->Cx = P->Len;
    P->Len += L->Len;
    EdDeleteLine(E, E->Cy);
    E->Cy--;
    E->Dirty = TRUE;
}

static VOID EdDelete(EDITOR *E)
{
    EDLINE *L = &E->Lines[E->Cy];
    EDLINE *N;
    UINTN I;

    if (E->Cx < L->Len) {
        MoveBytes(L->Data + E->Cx, L->Data + E->Cx + 1, L->Len - E->Cx - 1);
        L->Len--;
        E->Dirty = TRUE;
        return;
    }
    if (E->Cy + 1 >= E->Count) {
        return;
    }
    N = &E->Lines[E->Cy + 1];
    if (!LineReserve(L, L->Len + N->Len + 1)) {
        return;
    }
    for (I = 0; I < N->Len; I++) {
        L->Data[L->Len + I] = N->Data[I];
    }
    L->Len += N->Len;
    EdDeleteLine(E, E->Cy + 1);
    E->Dirty = TRUE;
}

static VOID EdCut(EDITOR *E)
{
    EDLINE *L = &E->Lines[E->Cy];
    UINTN I;

    E->Clip.Len = 0;
    if (LineReserve(&E->Clip, L->Len ? L->Len : 1)) {
        for (I = 0; I < L->Len; I++) {
            E->Clip.Data[I] = L->Data[I];
        }
        E->Clip.Len = L->Len;
        E->HasClip = TRUE;
    }
    EdDeleteLine(E, E->Cy);
    if (E->Cy >= E->Count) {
        E->Cy = E->Count - 1;
    }
    E->Cx = 0;
    E->Dirty = TRUE;
}

static VOID EdPaste(EDITOR *E)
{
    if (!E->HasClip) {
        SPrint(E->Status, sizeof(E->Status), L"Clipboard is empty");
        return;
    }
    if (EdInsertLine(E, E->Cy, E->Clip.Data, E->Clip.Len)) {
        E->Cy++;
        E->Dirty = TRUE;
    }
}

static VOID EdMoveY(EDITOR *E, INTN Delta)
{
    INTN Y = (INTN)E->Cy + Delta;

    if (Y < 0) {
        Y = 0;
    }
    if (Y >= (INTN)E->Count) {
        Y = (INTN)E->Count - 1;
    }
    E->Cy = (UINTN)Y;
    E->Cx = E->WantX < E->Lines[E->Cy].Len ? E->WantX : E->Lines[E->Cy].Len;
}

static VOID EdQuery(EDITOR *E)
{
    UINTN Cols = 80;
    UINTN Rows = 25;
    UINTN Need;

    uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, (UINTN)ST->ConOut->Mode->Mode, &Cols, &Rows);
    E->Cols = Cols;
    E->Rows = Rows;

    Need = (Cols + 2) * sizeof(CHAR16);
    if (Need > E->BufCap) {
        if (E->Row) {
            FreePool(E->Row);
        }
        if (E->Tmp) {
            FreePool(E->Tmp);
        }
        E->Row = AllocatePool(Need);
        E->Tmp = AllocatePool(Need);
        E->BufCap = Need;
    }
}

static VOID EdPutRow(EDITOR *E, UINTN Y, BOOLEAN Inverse, CHAR16 *Text)
{
    UINTN W = E->Cols - 1;
    UINTN I;

    for (I = 0; I < W && Text[I]; I++) {
        E->Row[I] = Text[I];
    }
    for (; I < W; I++) {
        E->Row[I] = L' ';
    }
    E->Row[W] = 0;

    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, Inverse ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY) : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, (UINTN)0, Y);
    Out(E->Row);
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
}

static VOID EdScroll(EDITOR *E)
{
    UINTN TH;

    EdQuery(E);
    TH = E->Rows - 3;
    if (E->Cy < E->Top) {
        E->Top = E->Cy;
    }
    if (E->Cy >= E->Top + TH) {
        E->Top = E->Cy - TH + 1;
    }
    if (E->Cx < E->Left) {
        E->Left = E->Cx;
    }
    if (E->Cx + 2 > E->Left + E->Cols) {
        E->Left = E->Cx + 2 - E->Cols;
    }
}

static VOID EdDraw(EDITOR *E)
{
    UINTN TH = E->Rows - 3;
    UINTN R;
    UINTN C;
    UINTN Idx;
    UINTN Pos;
    EDLINE *L;
    UINT8 B;
    UINTN TmpChars = E->BufCap / sizeof(CHAR16);

    uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);

    SPrint(E->Tmp, E->BufCap, L" IzhOS Notepad    %s%s", E->HasPath ? E->Path : L"New Buffer", E->Dirty ? L"  [Modified]" : L"");
    EdPutRow(E, 0, TRUE, E->Tmp);

    for (R = 0; R < TH; R++) {
        Idx = E->Top + R;
        for (C = 0; C + 1 < TmpChars && C < E->Cols - 1; C++) {
            E->Tmp[C] = L' ';
        }
        E->Tmp[C] = 0;
        if (Idx < E->Count) {
            L = &E->Lines[Idx];
            for (C = 0; C < E->Cols - 1; C++) {
                Pos = E->Left + C;
                if (Pos >= L->Len) {
                    break;
                }
                B = (UINT8)L->Data[Pos];
                if (B >= 32 && B < 127) {
                    E->Tmp[C] = (CHAR16)B;
                } else if (B == '\t') {
                    E->Tmp[C] = L' ';
                } else {
                    E->Tmp[C] = L'?';
                }
            }
        }
        EdPutRow(E, R + 1, FALSE, E->Tmp);
    }

    if (E->Status[0]) {
        SPrint(E->Tmp, E->BufCap, L" %s", E->Status);
    } else {
        SPrint(E->Tmp, E->BufCap, L" Line %d, Col %d  (%d lines)", (INTN)(E->Cy + 1), (INTN)(E->Cx + 1), (INTN)E->Count);
    }
    EdPutRow(E, E->Rows - 2, FALSE, E->Tmp);

    SPrint(E->Tmp, E->BufCap, L" ^O Save  ^X Exit  ^K Cut  ^U Paste  ^W Find   F2 Save  F10 Exit");
    EdPutRow(E, E->Rows - 1, TRUE, E->Tmp);

    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, E->Cx - E->Left, E->Cy - E->Top + 1);
    uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, TRUE);
}

static BOOLEAN EdPrompt(EDITOR *E, CHAR16 *Label, CHAR16 *Buf, UINTN Max)
{
    UINTN Len = StrLen(Buf);
    UINTN Col;
    EFI_INPUT_KEY K;

    for (;;) {
        EdQuery(E);
        SPrint(E->Tmp, E->BufCap, L" %s: %s", Label, Buf);
        EdPutRow(E, E->Rows - 2, TRUE, E->Tmp);
        Col = StrLen(Label) + 3 + Len;
        if (Col > E->Cols - 2) {
            Col = E->Cols - 2;
        }
        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, Col, E->Rows - 2);
        K = GetKey();
        if (K.UnicodeChar == L'\r' || K.UnicodeChar == L'\n') {
            return TRUE;
        }
        if (K.ScanCode == 0x17 || K.UnicodeChar == 3) {
            return FALSE;
        }
        if (K.UnicodeChar == 8) {
            if (Len > 0) {
                Len--;
                Buf[Len] = 0;
            }
        } else if (K.UnicodeChar >= 32 && K.UnicodeChar < 127 && Len + 1 < Max) {
            Buf[Len++] = K.UnicodeChar;
            Buf[Len] = 0;
        }
    }
}

static INTN EdAsk(EDITOR *E, CHAR16 *Message)
{
    EFI_INPUT_KEY K;

    for (;;) {
        EdQuery(E);
        SPrint(E->Tmp, E->BufCap, L" %s", Message);
        EdPutRow(E, E->Rows - 2, TRUE, E->Tmp);
        K = GetKey();
        if (K.UnicodeChar == L'y' || K.UnicodeChar == L'Y') {
            return 1;
        }
        if (K.UnicodeChar == L'n' || K.UnicodeChar == L'N') {
            return 0;
        }
        if (K.ScanCode == 0x17 || K.UnicodeChar == 3) {
            return -1;
        }
    }
}

static BOOLEAN EdSave(EDITOR *E)
{
    CHAR16 Name[MAXP];
    EFI_FILE_HANDLE File;
    EFI_STATUS Status;
    CHAR8 *Buf;
    UINTN Total = 0;
    UINTN Pos = 0;
    UINTN I;
    UINTN J;
    UINTN NL = E->CrLf ? 2 : 1;

    if (!E->HasPath) {
        Name[0] = 0;
        if (!EdPrompt(E, L"File name to write", Name, MAXP) || Name[0] == 0) {
            SPrint(E->Status, sizeof(E->Status), L"Cancelled");
            return FALSE;
        }
        if (!Resolve(Name, E->Path)) {
            SPrint(E->Status, sizeof(E->Status), L"Path too long");
            return FALSE;
        }
        E->HasPath = TRUE;
    }

    for (I = 0; I < E->Count; I++) {
        Total += E->Lines[I].Len;
        if (I + 1 < E->Count) {
            Total += NL;
        }
    }
    Buf = AllocatePool(Total + 1);
    if (Buf == NULL) {
        SPrint(E->Status, sizeof(E->Status), L"Out of memory");
        return FALSE;
    }
    for (I = 0; I < E->Count; I++) {
        for (J = 0; J < E->Lines[I].Len; J++) {
            Buf[Pos++] = E->Lines[I].Data[J];
        }
        if (I + 1 < E->Count) {
            if (E->CrLf) {
                Buf[Pos++] = '\r';
            }
            Buf[Pos++] = '\n';
        }
    }

    if (!EFI_ERROR(OpenAbs(E->Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &File))) {
        uefi_call_wrapper(File->Delete, 1, File);
    }
    Status = OpenAbs(E->Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &File);
    if (!EFI_ERROR(Status)) {
        if (Total > 0) {
            I = Total;
            Status = uefi_call_wrapper(File->Write, 3, File, &I, Buf);
        }
        CloseFile(File);
    }
    FreePool(Buf);

    if (EFI_ERROR(Status)) {
        SPrint(E->Status, sizeof(E->Status), L"Error writing file: %r", Status);
        return FALSE;
    }
    E->Dirty = FALSE;
    SPrint(E->Status, sizeof(E->Status), L"Wrote %d lines", (INTN)E->Count);
    return TRUE;
}

static BOOLEAN EdLoad(EDITOR *E, CHAR16 *Abs)
{
    UINT8 *Data;
    UINTN Size;
    UINTN I;
    UINTN Start = 0;
    UINTN End;
    BOOLEAN IsDir;
    EFI_STATUS Status;

    StrCpy(E->Path, Abs);
    E->HasPath = TRUE;

    if (!Exists(Abs, &IsDir)) {
        SPrint(E->Status, sizeof(E->Status), L"New file");
        return EdInsertLine(E, 0, NULL, 0);
    }
    if (IsDir) {
        Print(L"%s: is a directory\n", Abs);
        return FALSE;
    }
    Status = ReadWhole(Abs, &Data, &Size);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Abs, Status);
        return FALSE;
    }

    for (I = 0; I < Size; I++) {
        if (Data[I] == '\n') {
            End = I;
            if (End > Start && Data[End - 1] == '\r') {
                End--;
                E->CrLf = TRUE;
            }
            if (!EdInsertLine(E, E->Count, (CHAR8 *)Data + Start, End - Start)) {
                FreePool(Data);
                return FALSE;
            }
            Start = I + 1;
        }
    }
    if (!EdInsertLine(E, E->Count, (CHAR8 *)Data + Start, Size - Start)) {
        FreePool(Data);
        return FALSE;
    }
    FreePool(Data);
    return TRUE;
}

static UINTN FindInLine(EDLINE *L, CHAR8 *Pat, UINTN PLen, UINTN From)
{
    UINTN I;
    UINTN J;
    CHAR8 A;
    CHAR8 B;

    if (PLen == 0 || L->Len < PLen) {
        return NPOS;
    }
    for (I = From; I + PLen <= L->Len; I++) {
        for (J = 0; J < PLen; J++) {
            A = L->Data[I + J];
            B = Pat[J];
            if (A >= 'A' && A <= 'Z') {
                A = (CHAR8)(A + 32);
            }
            if (B >= 'A' && B <= 'Z') {
                B = (CHAR8)(B + 32);
            }
            if (A != B) {
                break;
            }
        }
        if (J == PLen) {
            return I;
        }
    }
    return NPOS;
}

static VOID EdSearch(EDITOR *E)
{
    CHAR8 Pat[128];
    UINTN PLen;
    UINTN K;
    UINTN Idx;
    UINTN From;
    UINTN Found;

    if (!EdPrompt(E, L"Search", E->Search, 128) || E->Search[0] == 0) {
        SPrint(E->Status, sizeof(E->Status), L"Cancelled");
        return;
    }
    PLen = StrLen(E->Search);
    for (K = 0; K < PLen; K++) {
        Pat[K] = (CHAR8)E->Search[K];
    }
    for (K = 0; K <= E->Count; K++) {
        Idx = (E->Cy + K) % E->Count;
        From = (K == 0) ? E->Cx + 1 : 0;
        Found = FindInLine(&E->Lines[Idx], Pat, PLen, From);
        if (Found != NPOS) {
            E->Cy = Idx;
            E->Cx = Found;
            SPrint(E->Status, sizeof(E->Status), L"Found");
            return;
        }
    }
    SPrint(E->Status, sizeof(E->Status), L"\"%s\" not found", E->Search);
}

static BOOLEAN EdQuit(EDITOR *E)
{
    INTN Answer;

    if (!E->Dirty) {
        return TRUE;
    }
    Answer = EdAsk(E, L"Save modified buffer?  (Y = yes, N = no, Esc = cancel)");
    if (Answer < 0) {
        return FALSE;
    }
    if (Answer == 0) {
        return TRUE;
    }
    return EdSave(E);
}

static VOID CmdNotepad(INTN Argc, CHAR16 **Argv)
{
    EDITOR *E;
    CHAR16 Abs[MAXP];
    EFI_INPUT_KEY K;
    BOOLEAN Done = FALSE;
    BOOLEAN Vert;
    UINTN I;
    UINTN Page;

    E = AllocateZeroPool(sizeof(EDITOR));
    if (E == NULL) {
        Print(L"Out of memory\n");
        return;
    }

    EdQuery(E);
    if (E->Cols < 30 || E->Rows < 6 || E->Row == NULL || E->Tmp == NULL) {
        Print(L"Console is too small for notepad\n");
        if (E->Row) {
            FreePool(E->Row);
        }
        if (E->Tmp) {
            FreePool(E->Tmp);
        }
        FreePool(E);
        return;
    }

    if (Argc > 1) {
        if (!Resolve(Argv[1], Abs) || !EdLoad(E, Abs)) {
            Done = TRUE;
        }
    } else {
        EdInsertLine(E, 0, NULL, 0);
    }

    if (E->Count == 0) {
        Done = TRUE;
    }

    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    while (!Done) {
        EdScroll(E);
        EdDraw(E);
        K = GetKey();
        E->Status[0] = 0;
        Vert = FALSE;
        Page = E->Rows - 3;

        if (K.ScanCode != 0) {
            switch (K.ScanCode) {
            case 1:
                EdMoveY(E, -1);
                Vert = TRUE;
                break;
            case 2:
                EdMoveY(E, 1);
                Vert = TRUE;
                break;
            case 3:
                if (E->Cx < E->Lines[E->Cy].Len) {
                    E->Cx++;
                } else if (E->Cy + 1 < E->Count) {
                    E->Cy++;
                    E->Cx = 0;
                }
                break;
            case 4:
                if (E->Cx > 0) {
                    E->Cx--;
                } else if (E->Cy > 0) {
                    E->Cy--;
                    E->Cx = E->Lines[E->Cy].Len;
                }
                break;
            case 5:
                E->Cx = 0;
                break;
            case 6:
                E->Cx = E->Lines[E->Cy].Len;
                break;
            case 8:
                EdDelete(E);
                break;
            case 9:
                EdMoveY(E, -(INTN)Page);
                Vert = TRUE;
                break;
            case 10:
                EdMoveY(E, (INTN)Page);
                Vert = TRUE;
                break;
            case 0x0C:
                EdSave(E);
                break;
            case 0x14:
                if (EdQuit(E)) {
                    Done = TRUE;
                }
                break;
            default:
                break;
            }
        } else {
            switch (K.UnicodeChar) {
            case 0x0F:
                EdSave(E);
                break;
            case 0x18:
                if (EdQuit(E)) {
                    Done = TRUE;
                }
                break;
            case 0x0B:
                EdCut(E);
                break;
            case 0x15:
                EdPaste(E);
                break;
            case 0x17:
                EdSearch(E);
                break;
            case 0x0D:
            case 0x0A:
                EdNewline(E);
                break;
            case 0x08:
                EdBackspace(E);
                break;
            case 0x09:
                for (I = 0; I < 4; I++) {
                    EdInsertChar(E, ' ');
                }
                break;
            default:
                if (K.UnicodeChar >= 32 && K.UnicodeChar < 127) {
                    EdInsertChar(E, (CHAR8)K.UnicodeChar);
                }
                break;
            }
        }

        if (!Vert) {
            E->WantX = E->Cx;
        }
    }

    for (I = 0; I < E->Count; I++) {
        if (E->Lines[I].Data) {
            FreePool(E->Lines[I].Data);
        }
    }
    if (E->Lines) {
        FreePool(E->Lines);
    }
    if (E->Clip.Data) {
        FreePool(E->Clip.Data);
    }
    FreePool(E->Row);
    FreePool(E->Tmp);
    FreePool(E);

    ResetConsole();
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static BOOLEAN IsRegular(CHAR16 *Path)
{
    BOOLEAN IsDir;

    return Exists(Path, &IsDir) && !IsDir;
}

static BOOLEAN FindExecutable(CHAR16 *Cmd, CHAR16 *Result)
{
    CHAR16 Abs[MAXP];
    BOOLEAN HasSep = FALSE;
    UINTN I;

    for (I = 0; Cmd[I]; I++) {
        if (IsSep(Cmd[I])) {
            HasSep = TRUE;
        }
    }

    if (Resolve(Cmd, Abs)) {
        if (IsRegular(Abs)) {
            StrCpy(Result, Abs);
            return TRUE;
        }
        if (StrLen(Abs) + 4 < MAXP) {
            StrCat(Abs, L".efi");
            if (IsRegular(Abs)) {
                StrCpy(Result, Abs);
                return TRUE;
            }
        }
    }

    if (!HasSep && StrLen(Cmd) + StrLen(SYSTEM_DIR) + 6 < MAXP) {
        StrCpy(Abs, SYSTEM_DIR);
        StrCat(Abs, L"\\");
        StrCat(Abs, Cmd);
        if (IsRegular(Abs)) {
            StrCpy(Result, Abs);
            return TRUE;
        }
        StrCat(Abs, L".efi");
        if (IsRegular(Abs)) {
            StrCpy(Result, Abs);
            return TRUE;
        }
    }
    return FALSE;
}

static VOID RunExternal(CHAR16 *Cmd, CHAR16 *Args)
{
    CHAR16 Path[MAXP];
    EFI_DEVICE_PATH *Dp;
    EFI_HANDLE Image = NULL;
    EFI_LOADED_IMAGE *Li = NULL;
    EFI_STATUS Status;
    UINTN ExitSize = 0;
    CHAR16 *ExitData = NULL;
    CHAR16 *Options = NULL;

    if (!FindExecutable(Cmd, Path)) {
        Print(L"'%s' is not recognized as a command or program\n", Cmd);
        return;
    }

    Dp = FileDevicePath(gDevice, Path);
    if (Dp == NULL) {
        Print(L"Out of memory\n");
        return;
    }
    Status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, gImage, Dp, NULL, (UINTN)0, &Image);
    FreePool(Dp);
    if (EFI_ERROR(Status)) {
        Print(L"%s: cannot load: %r\n", Cmd, Status);
        return;
    }

    if (Args[0] != 0) {
        Status = uefi_call_wrapper(BS->HandleProtocol, 3, Image, &LoadedImageProtocol, (VOID **)&Li);
        if (!EFI_ERROR(Status) && Li != NULL) {
            Options = StrDuplicate(Args);
            if (Options != NULL) {
                Li->LoadOptions = Options;
                Li->LoadOptionsSize = (UINT32)StrSize(Options);
            }
        }
    }

    Status = uefi_call_wrapper(BS->StartImage, 3, Image, &ExitSize, &ExitData);
    if (ExitData != NULL) {
        FreePool(ExitData);
    }
    if (Options != NULL) {
        FreePool(Options);
    }
    ResetConsole();
    if (EFI_ERROR(Status)) {
        Print(L"\n%s exited with status: %r\n", Cmd, Status);
    }
}

static BOOLEAN LoadArg(CHAR16 *Arg, UINT8 **Data, UINTN *Size)
{
    CHAR16 Abs[MAXP];
    EFI_STATUS Status;

    if (!Resolve(Arg, Abs)) {
        Print(L"Path too long\n");
        return FALSE;
    }
    Status = ReadWhole(Abs, Data, Size);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Arg, Status);
        return FALSE;
    }
    return TRUE;
}

static VOID PutText(UINT8 *D, UINTN Len)
{
    CHAR16 Buf[256];
    UINTN N = 0;
    UINTN I;

    for (I = 0; I < Len; I++) {
        if (D[I] == '\r') {
            continue;
        }
        if (D[I] == '\n') {
            Buf[N++] = L'\r';
            Buf[N++] = L'\n';
        } else if (D[I] >= 32 && D[I] < 127) {
            Buf[N++] = (CHAR16)D[I];
        } else if (D[I] == '\t') {
            Buf[N++] = L' ';
        } else {
            Buf[N++] = L'?';
        }
        if (N >= 250) {
            Buf[N] = 0;
            Out(Buf);
            N = 0;
        }
    }
    Buf[N] = 0;
    Out(Buf);
}

static VOID CmdRen(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Src[MAXP];
    CHAR16 Parent[MAXP];
    CHAR16 Name[MAXP];
    CHAR16 To[MAXP];
    EFI_STATUS Status;

    if (Argc < 3) {
        Print(L"Usage: ren <name> <newname>\n");
        return;
    }
    if (!Resolve(Argv[1], Src) || !Exists(Src, NULL)) {
        Print(L"%s: %r\n", Argv[1], EFI_NOT_FOUND);
        return;
    }
    SplitPath(Src, Parent, Name);
    if (!JoinPath(Parent, Argv[2], To)) {
        Print(L"Path too long\n");
        return;
    }
    if (Exists(To, NULL)) {
        Print(L"%s: already exists\n", Argv[2]);
        return;
    }
    Status = MoveOne(Src, To);
    if (EFI_ERROR(Status)) {
        Print(L"%s: %r\n", Argv[1], Status);
    }
}

static VOID TreeWalk(CHAR16 *Dir, UINTN Depth)
{
    EFI_FILE_HANDLE H;
    NAMELIST L;
    CHAR16 Child[MAXP];
    UINTN I;
    UINTN J;

    ZeroMem(&L, sizeof(L));
    if (Depth > 12 || EFI_ERROR(OpenAbs(Dir, EFI_FILE_MODE_READ, 0, &H))) {
        return;
    }
    ReadDir(H, &L, NULL);
    CloseFile(H);
    for (I = 0; I < L.Count; I++) {
        for (J = 0; J < Depth; J++) {
            Out(L"|   ");
        }
        Print(L"+-- %s%s\n", L.Names[I], L.IsDir[I] ? L"\\" : L"");
        if (L.IsDir[I] && JoinPath(Dir, L.Names[I], Child)) {
            TreeWalk(Child, Depth + 1);
        }
    }
    ListFree(&L);
}

static VOID CmdTree(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    BOOLEAN IsDir = FALSE;

    if (!Resolve(Argc > 1 ? Argv[1] : L"", Abs) || !Exists(Abs, &IsDir) || !IsDir) {
        Print(L"Invalid directory\n");
        return;
    }
    Print(L"%s\n", Abs);
    TreeWalk(Abs, 0);
}

static VOID CmdTouch(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Abs[MAXP];
    EFI_FILE_HANDLE F;
    EFI_STATUS Status;
    INTN A;

    if (Argc < 2) {
        Print(L"Usage: touch <file> ...\n");
        return;
    }
    for (A = 1; A < Argc; A++) {
        if (!Resolve(Argv[A], Abs) || Exists(Abs, NULL)) {
            continue;
        }
        Status = OpenAbs(Abs, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &F);
        if (EFI_ERROR(Status)) {
            Print(L"%s: %r\n", Argv[A], Status);
        } else {
            CloseFile(F);
        }
    }
}

static VOID CmdWc(INTN Argc, CHAR16 **Argv)
{
    UINT8 *Data;
    UINTN Size;
    UINTN I;
    UINTN Lines = 0;
    UINTN Words = 0;
    BOOLEAN In = FALSE;

    if (Argc < 2) {
        Print(L"Usage: wc <file>\n");
        return;
    }
    if (!LoadArg(Argv[1], &Data, &Size)) {
        return;
    }
    for (I = 0; I < Size; I++) {
        if (Data[I] == '\n') {
            Lines++;
        }
        if (Data[I] == ' ' || Data[I] == '\n' || Data[I] == '\t' || Data[I] == '\r') {
            In = FALSE;
        } else if (!In) {
            In = TRUE;
            Words++;
        }
    }
    Print(L"%lu lines  %lu words  %lu bytes\n", (UINT64)Lines, (UINT64)Words, (UINT64)Size);
    FreePool(Data);
}

static VOID CmdHeadTail(INTN Argc, CHAR16 **Argv, BOOLEAN Tail)
{
    UINT8 *Data;
    UINTN Size;
    UINTN Want = 10;
    UINTN Start = 0;
    UINTN End;
    UINTN Seen = 0;

    if (Argc < 2) {
        Print(L"Usage: %s <file> [lines]\n", Tail ? L"tail" : L"head");
        return;
    }
    if (Argc > 2) {
        Want = (UINTN)Atoi(Argv[2]);
    }
    if (Want == 0) {
        Want = 1;
    }
    if (!LoadArg(Argv[1], &Data, &Size)) {
        return;
    }
    End = Size;
    if (Tail) {
        Start = Size;
        if (Start > 0 && Data[Start - 1] == '\n') {
            Start--;
        }
        while (Start > 0) {
            if (Data[Start - 1] == '\n' && ++Seen == Want) {
                break;
            }
            Start--;
        }
    } else {
        for (End = 0; End < Size; End++) {
            if (Data[End] == '\n' && ++Seen >= Want) {
                End++;
                break;
            }
        }
    }
    PutText(Data + Start, End - Start);
    if (End > Start && Data[End - 1] != '\n') {
        Out(L"\r\n");
    }
    FreePool(Data);
}

static VOID CmdHex(INTN Argc, CHAR16 **Argv)
{
    CHAR16 Asc[17];
    UINT8 *Data;
    UINTN Size;
    UINTN Max;
    UINTN Limit = 256;
    UINTN I;
    UINTN J;

    if (Argc < 2) {
        Print(L"Usage: hex <file> [bytes]\n");
        return;
    }
    if (Argc > 2) {
        Limit = (UINTN)Atoi(Argv[2]);
    }
    if (!LoadArg(Argv[1], &Data, &Size)) {
        return;
    }
    Max = Size < Limit ? Size : Limit;
    for (I = 0; I < Max; I += 16) {
        Print(L"%08x  ", I);
        for (J = 0; J < 16; J++) {
            if (I + J < Max) {
                Print(L"%02x ", (UINTN)Data[I + J]);
                Asc[J] = (Data[I + J] >= 32 && Data[I + J] < 127) ? (CHAR16)Data[I + J] : L'.';
            } else {
                Out(L"   ");
                Asc[J] = L' ';
            }
        }
        Asc[16] = 0;
        Print(L" %s\n", Asc);
    }
    FreePool(Data);
}

static VOID CmdMem(VOID)
{
    UINTN Entries = 0;
    UINTN Key = 0;
    UINTN DescSize = 0;
    UINT32 Ver = 0;
    EFI_MEMORY_DESCRIPTOR *Map;
    EFI_MEMORY_DESCRIPTOR *D;
    UINT64 Free = 0;
    UINT64 Total = 0;
    UINTN I;

    Map = LibMemoryMap(&Entries, &Key, &DescSize, &Ver);
    if (Map == NULL) {
        Print(L"Memory map unavailable\n");
        return;
    }
    for (I = 0; I < Entries; I++) {
        D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + I * DescSize);
        if (D->Type == EfiConventionalMemory) {
            Free += D->NumberOfPages;
            Total += D->NumberOfPages;
        } else if (D->Type != EfiReservedMemoryType && D->Type != EfiUnusableMemory && D->Type != EfiMemoryMappedIO && D->Type != EfiMemoryMappedIOPortSpace && D->Type != EfiPalCode) {
            Total += D->NumberOfPages;
        }
    }
    FreePool(Map);
    Print(L"Total: %lu MB\nFree:  %lu MB\nUsed:  %lu MB\n", Total / 256, Free / 256, (Total - Free) / 256);
}

static VOID CmdClock(BOOLEAN Date)
{
    EFI_TIME T;

    if (EFI_ERROR(uefi_call_wrapper(RT->GetTime, 2, &T, NULL))) {
        Print(L"Clock unavailable\n");
        return;
    }
    if (Date) {
        Print(L"%04d-%02d-%02d\n", (INTN)T.Year, (INTN)T.Month, (INTN)T.Day);
    } else {
        Print(L"%02d:%02d:%02d\n", (INTN)T.Hour, (INTN)T.Minute, (INTN)T.Second);
    }
}

static VOID CmdVol(VOID)
{
    EFI_FILE_SYSTEM_INFO *Info = LibFileSystemInfo(gRoot);

    if (Info == NULL) {
        Print(L"Volume information unavailable\n");
        return;
    }
    Print(L"Label: %s\nSize:  %lu bytes\nFree:  %lu bytes\n", Info->VolumeLabel, Info->VolumeSize, Info->FreeSpace);
    FreePool(Info);
}

static VOID CmdColor(INTN Argc, CHAR16 **Argv)
{
    UINTN Fg = EFI_LIGHTGRAY;
    UINTN Bg = EFI_BLACK;

    if (Argc > 1) {
        Fg = (UINTN)Atoi(Argv[1]) & 0x0F;
    }
    if (Argc > 2) {
        Bg = (UINTN)Atoi(Argv[2]) & 0x07;
    }
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_TEXT_ATTR(Fg, Bg));
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static VOID CmdSleep(INTN Argc, CHAR16 **Argv)
{
    INTN N = Argc > 1 ? Atoi(Argv[1]) : 1;
    INTN I;

    for (I = 0; I < N; I++) {
        uefi_call_wrapper(BS->Stall, 1, (UINTN)1000000);
    }
}

static VOID CmdHelp(VOID)
{
    Print(L"IzhOS Command Shell %s\n\n", SHELL_VERSION);
    Print(L"  dir [path|pattern]        list directory contents\n");
    Print(L"  cd [path]                 change or show current directory\n");
    Print(L"  type <file>               print a file\n");
    Print(L"  copy <src> <dst>          copy files or directories\n");
    Print(L"  move <src> <dst>          move or rename files or directories\n");
    Print(L"  del <file|pattern> ...    delete files\n");
    Print(L"  rmdir [/s] <dir> ...      remove directories (rd)\n");
    Print(L"  mkdir <dir> ...           create directories (md)\n");
    Print(L"  find <name> [dir]         search for files by name\n");
    Print(L"  find /t <text> <file>     search for text inside files\n");
    Print(L"  notepad [file]            text editor\n");
    Print(L"  ren <name> <new>          rename a file or directory (rename)\n");
    Print(L"  pwd                       print current directory\n");
    Print(L"  tree [dir]                show directory tree\n");
    Print(L"  touch <file> ...          create empty files\n");
    Print(L"  wc <file>                 count lines, words, bytes\n");
    Print(L"  head|tail <file> [n]      show first or last n lines\n");
    Print(L"  hex <file> [bytes]        hex dump\n");
    Print(L"  mem                       memory usage\n");
    Print(L"  date / time               show date or time\n");
    Print(L"  vol                       volume label and space\n");
    Print(L"  color [fg] [bg]           set text colors (0-15, 0-7)\n");
    Print(L"  sleep [seconds]           wait\n");
    Print(L"  pause                     wait for a key\n");
    Print(L"  echo <text>               print text\n");
    Print(L"  cls                       clear the screen\n");
    Print(L"  ver                       show version\n");
    Print(L"  reboot                    restart the machine\n");
    Print(L"  exit                      shut down\n");
    Print(L"  a | b    a > file    a >> file    a < file    a ; b    a && b    a || b    a & b\n");
    Print(L"  pipe filters: cat, sort [/r], find /t <text>, wc, head [n], tail [n]\n");
    Print(L"\nAny .efi program can be run by typing its name.\n\n");
}

static VOID Tokenize(CHAR16 *Line, CHAR16 **Argv, INTN *Argc)
{
    CHAR16 *P = Line;

    *Argc = 0;
    while (*P) {
        while (*P == L' ' || *P == L'\t') {
            P++;
        }
        if (!*P || *Argc >= MAXARGS) {
            break;
        }
        if (*P == L'"') {
            P++;
            Argv[(*Argc)++] = P;
            while (*P && *P != L'"') {
                P++;
            }
        } else {
            Argv[(*Argc)++] = P;
            while (*P && *P != L' ' && *P != L'\t') {
                P++;
            }
        }
        if (*P) {
            *P++ = 0;
        }
    }
}

static VOID ProcessSimple(CHAR16 *Line)
{
    CHAR16 Work[MAXLINE + 4];
    CHAR16 Raw[MAXLINE + 4];
    CHAR16 *Argv[MAXARGS];
    CHAR16 *P;
    INTN Argc;

    P = Line;
    while (*P == L' ' || *P == L'\t') {
        P++;
    }
    if (*P == 0) {
        return;
    }

    if ((P[0] == L'c' || P[0] == L'C') && (P[1] == L'd' || P[1] == L'D') && (IsSep(P[2]) || P[2] == L'.')) {
        StrCpy(Work, L"cd ");
        StrCat(Work, P + 2);
    } else {
        StrCpy(Work, P);
    }

    P = Work;
    if (*P == L'"') {
        P++;
        while (*P && *P != L'"') {
            P++;
        }
        if (*P) {
            P++;
        }
    } else {
        while (*P && *P != L' ' && *P != L'\t') {
            P++;
        }
    }
    while (*P == L' ' || *P == L'\t') {
        P++;
    }
    StrCpy(Raw, P);

    Tokenize(Work, Argv, &Argc);
    if (Argc == 0) {
        return;
    }

    if (!StriCmp(Argv[0], L"dir")) {
        CmdDir(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"cd") || !StriCmp(Argv[0], L"chdir")) {
        CmdCd(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"cls") || !StriCmp(Argv[0], L"clear")) {
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    } else if (!StriCmp(Argv[0], L"del") || !StriCmp(Argv[0], L"erase")) {
        CmdDel(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"rmdir") || !StriCmp(Argv[0], L"rd")) {
        CmdRmdir(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"mkdir") || !StriCmp(Argv[0], L"md")) {
        CmdMkdir(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"copy")) {
        CmdCopy(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"move")) {
        CmdMove(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"type")) {
        CmdType(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"find")) {
        CmdFind(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"notepad")) {
        CmdNotepad(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"ren") || !StriCmp(Argv[0], L"rename")) {
        CmdRen(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"pwd")) {
        Print(L"%s\n", gCwd);
    } else if (!StriCmp(Argv[0], L"tree")) {
        CmdTree(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"touch")) {
        CmdTouch(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"wc")) {
        CmdWc(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"head")) {
        CmdHeadTail(Argc, Argv, FALSE);
    } else if (!StriCmp(Argv[0], L"tail")) {
        CmdHeadTail(Argc, Argv, TRUE);
    } else if (!StriCmp(Argv[0], L"hex")) {
        CmdHex(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"mem")) {
        CmdMem();
    } else if (!StriCmp(Argv[0], L"date")) {
        CmdClock(TRUE);
    } else if (!StriCmp(Argv[0], L"time")) {
        CmdClock(FALSE);
    } else if (!StriCmp(Argv[0], L"vol")) {
        CmdVol();
    } else if (!StriCmp(Argv[0], L"pause")) {
        Print(L"Press any key to continue . . .");
        GetKey();
        Out(L"\r\n");
    } else if (!StriCmp(Argv[0], L"color")) {
        CmdColor(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"sleep")) {
        CmdSleep(Argc, Argv);
    } else if (!StriCmp(Argv[0], L"echo")) {
        Print(L"%s\n", Raw);
    } else if (!StriCmp(Argv[0], L"ver")) {
        Print(L"IzhOS Command Shell %s\n", SHELL_VERSION);
    } else if (!StriCmp(Argv[0], L"help") || !StriCmp(Argv[0], L"?")) {
        CmdHelp();
    } else if (!StriCmp(Argv[0], L"exit") || !StriCmp(Argv[0], L"shutdown")) {
        gExit = TRUE;
    } else if (!StriCmp(Argv[0], L"reboot")) {
        uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, (UINTN)0, NULL);
    } else {
        RunExternal(Argv[0], Raw);
    }
}

typedef struct {
    UINT8 *Data;
    UINTN Len;
    UINTN Cap;
} OpBuf;

typedef struct {
    CHAR16 *Text;
    INTN Op;
} OpSeg;

#define OP_NONE 0
#define OP_SEMI 1
#define OP_AMP 2
#define OP_AND 3
#define OP_OR 4
#define OP_PIPE 5
#define OP_MAXSEG 64

static OpBuf *gOpCap;
static SIMPLE_TEXT_OUTPUT_INTERFACE gOpOut;
static SIMPLE_TEXT_OUTPUT_INTERFACE *gOpReal;
static UINT8 *gOpIn;
static UINTN gOpInLen;
static BOOLEAN gOpHasIn;

static VOID OpAdd(OpBuf *B, UINT8 C)
{
    UINT8 *N;
    UINTN Cap;

    if (B->Len + 1 >= B->Cap) {
        Cap = B->Cap ? B->Cap * 2 : 1024;
        N = AllocatePool(Cap);
        if (N == NULL) {
            return;
        }
        if (B->Data != NULL) {
            CopyMem(N, B->Data, B->Len);
            FreePool(B->Data);
        }
        B->Data = N;
        B->Cap = Cap;
    }
    B->Data[B->Len++] = C;
}

static EFI_STATUS __attribute__((ms_abi)) OpWrite(SIMPLE_TEXT_OUTPUT_INTERFACE *This, CHAR16 *Str)
{
    if (gOpCap != NULL) {
        for (; *Str; Str++) {
            if (*Str != L'\r') {
                OpAdd(gOpCap, *Str < 128 ? (UINT8)*Str : (UINT8)'?');
            }
        }
    }
    return EFI_SUCCESS;
}

static VOID OpCapBegin(OpBuf *B)
{
    gOpCap = B;
    gOpReal = ST->ConOut;
    CopyMem(&gOpOut, gOpReal, sizeof(gOpOut));
    gOpOut.OutputString = (typeof(gOpOut.OutputString))OpWrite;
    ST->ConOut = &gOpOut;
}

static VOID OpCapEnd(VOID)
{
    ST->ConOut = gOpReal;
    gOpCap = NULL;
}

static VOID OpSetIn(UINT8 *D, UINTN L)
{
    if (gOpIn != NULL) {
        FreePool(gOpIn);
    }
    gOpIn = D;
    gOpInLen = L;
    gOpHasIn = TRUE;
}

static VOID OpClearIn(VOID)
{
    if (gOpIn != NULL) {
        FreePool(gOpIn);
    }
    gOpIn = NULL;
    gOpInLen = 0;
    gOpHasIn = FALSE;
}

static VOID OpPut(UINT8 *D, UINTN Len)
{
    CHAR16 T[130];
    UINTN N = 0;
    UINTN I;

    for (I = 0; I < Len; I++) {
        if (D[I] == '\n') {
            T[N++] = L'\r';
            T[N++] = L'\n';
        } else {
            T[N++] = D[I];
        }
        if (N >= 126) {
            T[N] = 0;
            Out(T);
            N = 0;
        }
    }
    T[N] = 0;
    Out(T);
}

static BOOLEAN OpLoad(CHAR16 *Arg)
{
    CHAR16 Abs[MAXP];
    UINT8 *D;
    UINTN L;
    EFI_STATUS S;

    if (!Resolve(Arg, Abs)) {
        Print(L"Path too long\n");
        return FALSE;
    }
    S = ReadWhole(Abs, &D, &L);
    if (EFI_ERROR(S)) {
        Print(L"%s: %r\n", Arg, S);
        return FALSE;
    }
    OpSetIn(D, L);
    return TRUE;
}

static BOOLEAN OpTable(UINTN **St, UINTN **En, UINTN *Count)
{
    UINTN N = 0;
    UINTN I;
    UINTN K = 0;
    UINTN Start = 0;
    UINTN E;

    for (I = 0; I < gOpInLen; I++) {
        if (gOpIn[I] == '\n') {
            N++;
        }
    }
    if (gOpInLen > 0 && gOpIn[gOpInLen - 1] != '\n') {
        N++;
    }
    *Count = N;
    *St = AllocatePool((N + 1) * sizeof(UINTN));
    *En = AllocatePool((N + 1) * sizeof(UINTN));
    if (*St == NULL || *En == NULL) {
        if (*St != NULL) {
            FreePool(*St);
        }
        if (*En != NULL) {
            FreePool(*En);
        }
        return FALSE;
    }
    for (I = 0; I <= gOpInLen && K < N; I++) {
        if (I == gOpInLen || gOpIn[I] == '\n') {
            E = I;
            if (E > Start && gOpIn[E - 1] == '\r') {
                E--;
            }
            (*St)[K] = Start;
            (*En)[K] = E;
            K++;
            Start = I + 1;
        }
    }
    return TRUE;
}

static VOID OpLine(UINTN S, UINTN E)
{
    OpPut(gOpIn + S, E - S);
    OpPut((UINT8 *)"\n", 1);
}

static INTN OpCmp(UINTN A, UINTN B, UINTN *St, UINTN *En)
{
    UINTN I = St[A];
    UINTN J = St[B];
    UINT8 X;
    UINT8 Y;

    while (I < En[A] && J < En[B]) {
        X = gOpIn[I];
        Y = gOpIn[J];
        if (X >= 'a' && X <= 'z') {
            X -= 32;
        }
        if (Y >= 'a' && Y <= 'z') {
            Y -= 32;
        }
        if (X != Y) {
            return X < Y ? -1 : 1;
        }
        I++;
        J++;
    }
    if (I < En[A]) {
        return 1;
    }
    if (J < En[B]) {
        return -1;
    }
    return 0;
}

static BOOLEAN OpIsNum(CHAR16 *S)
{
    if (*S == 0) {
        return FALSE;
    }
    for (; *S; S++) {
        if (*S < L'0' || *S > L'9') {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN OpFilter(CHAR16 *T)
{
    CHAR16 Work[MAXLINE + 4];
    CHAR16 Tmp[MAXLINE];
    CHAR16 *Argv[MAXARGS];
    CHAR16 *File = NULL;
    CHAR16 *Cmd;
    UINTN *St = NULL;
    UINTN *En = NULL;
    UINTN *Idx = NULL;
    UINTN N = 0;
    UINTN I;
    UINTN J;
    UINTN K;
    UINTN Want = 10;
    UINTN Words = 0;
    BOOLEAN Rev = FALSE;
    BOOLEAN In = FALSE;
    BOOLEAN Cat;
    BOOLEAN Find;
    BOOLEAN Sort;
    BOOLEAN Wc;
    BOOLEAN Head;
    BOOLEAN Tail;
    INTN Argc;
    INTN A;

    StrnCpy(Work, T, MAXLINE + 3);
    Work[MAXLINE + 3] = 0;
    Tokenize(Work, Argv, &Argc);
    if (Argc == 0) {
        return FALSE;
    }
    Cmd = Argv[0];
    Cat = !StriCmp(Cmd, L"cat") || (!StriCmp(Cmd, L"type") && Argc == 1 && gOpHasIn);
    Find = !StriCmp(Cmd, L"find") && Argc == 3 && !StriCmp(Argv[1], L"/t") && gOpHasIn;
    Sort = !StriCmp(Cmd, L"sort");
    Wc = gOpHasIn && !StriCmp(Cmd, L"wc");
    Head = gOpHasIn && !StriCmp(Cmd, L"head");
    Tail = gOpHasIn && !StriCmp(Cmd, L"tail");
    if (!Cat && !Find && !Sort && !Wc && !Head && !Tail) {
        return FALSE;
    }
    if (!Find) {
        for (A = 1; A < Argc; A++) {
            if (!StriCmp(Argv[A], L"/r")) {
                Rev = TRUE;
            } else if (OpIsNum(Argv[A])) {
                Want = (UINTN)Atoi(Argv[A]);
            } else {
                File = Argv[A];
            }
        }
    }
    if (File != NULL && !OpLoad(File)) {
        return TRUE;
    }
    if (!gOpHasIn) {
        Print(L"Usage: %s <file>  or  <command> | %s\n", Cmd, Cmd);
        return TRUE;
    }
    if (Cat) {
        OpPut(gOpIn, gOpInLen);
        return TRUE;
    }
    if (!OpTable(&St, &En, &N)) {
        Print(L"Out of memory\n");
        return TRUE;
    }

    if (Find) {
        for (I = 0; I < N; I++) {
            for (J = 0; J < En[I] - St[I] && J < MAXLINE - 1; J++) {
                Tmp[J] = (CHAR16)gOpIn[St[I] + J];
            }
            Tmp[J] = 0;
            if (ContainsNoCase(Tmp, Argv[2])) {
                OpLine(St[I], En[I]);
            }
        }
    } else if (Sort) {
        Idx = AllocatePool((N + 1) * sizeof(UINTN));
        if (Idx != NULL) {
            for (I = 0; I < N; I++) {
                K = I;
                while (K > 0 && (Rev ? OpCmp(Idx[K - 1], I, St, En) < 0 : OpCmp(Idx[K - 1], I, St, En) > 0)) {
                    Idx[K] = Idx[K - 1];
                    K--;
                }
                Idx[K] = I;
            }
            for (I = 0; I < N; I++) {
                OpLine(St[Idx[I]], En[Idx[I]]);
            }
            FreePool(Idx);
        }
    } else if (Wc) {
        for (I = 0; I < gOpInLen; I++) {
            if (gOpIn[I] == ' ' || gOpIn[I] == '\n' || gOpIn[I] == '\t' || gOpIn[I] == '\r') {
                In = FALSE;
            } else if (!In) {
                In = TRUE;
                Words++;
            }
        }
        Print(L"%lu lines  %lu words  %lu bytes\n", (UINT64)N, (UINT64)Words, (UINT64)gOpInLen);
    } else {
        I = Head ? 0 : (N > Want ? N - Want : 0);
        K = Head ? (Want < N ? Want : N) : N;
        for (; I < K; I++) {
            OpLine(St[I], En[I]);
        }
    }
    FreePool(St);
    FreePool(En);
    return TRUE;
}

static BOOLEAN OpWriteFile(CHAR16 *Name, OpBuf *B, BOOLEAN Append)
{
    CHAR16 Abs[MAXP];
    EFI_FILE_HANDLE F;
    EFI_STATUS S;
    UINTN N;

    if (!Resolve(Name, Abs)) {
        Print(L"Path too long\n");
        return FALSE;
    }
    if (!Append && !EFI_ERROR(OpenAbs(Abs, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &F))) {
        uefi_call_wrapper(F->Delete, 1, F);
    }
    S = OpenAbs(Abs, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &F);
    if (EFI_ERROR(S)) {
        Print(L"%s: %r\n", Name, S);
        return FALSE;
    }
    if (Append) {
        uefi_call_wrapper(F->SetPosition, 2, F, (UINT64)0xFFFFFFFFFFFFFFFFULL);
    }
    N = B->Len;
    if (N > 0) {
        S = uefi_call_wrapper(F->Write, 3, F, &N, B->Data);
    }
    CloseFile(F);
    if (EFI_ERROR(S)) {
        Print(L"%s: %r\n", Name, S);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN OpRedir(CHAR16 *T, CHAR16 *In, CHAR16 *OutName, BOOLEAN *Append)
{
    CHAR16 *P = T;
    CHAR16 *Dst;
    BOOLEAN InQ = FALSE;
    BOOLEAN Q;
    UINTN N;

    In[0] = 0;
    OutName[0] = 0;
    *Append = FALSE;
    while (*P) {
        if (*P == L'"') {
            InQ = !InQ;
            P++;
            continue;
        }
        if (InQ || (*P != L'<' && *P != L'>')) {
            P++;
            continue;
        }
        if (*P == L'<') {
            Dst = In;
        } else {
            Dst = OutName;
            *Append = (P[1] == L'>');
            if (*Append) {
                *P++ = L' ';
            }
        }
        *P++ = L' ';
        while (*P == L' ' || *P == L'\t') {
            P++;
        }
        Q = (*P == L'"');
        if (Q) {
            *P++ = L' ';
        }
        N = 0;
        while (*P && (Q ? *P != L'"' : (*P != L' ' && *P != L'\t' && *P != L'<' && *P != L'>'))) {
            if (N + 1 < MAXP) {
                Dst[N++] = *P;
            }
            *P++ = L' ';
        }
        Dst[N] = 0;
        if (Q && *P == L'"') {
            *P++ = L' ';
        }
        if (N == 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static INTN OpSplit(CHAR16 *Line, OpSeg *Segs)
{
    CHAR16 *P = Line;
    CHAR16 *Start = Line;
    BOOLEAN InQ = FALSE;
    INTN N = 0;
    INTN Op;
    INTN Skip;

    for (;; P++) {
        if (*P == L'"') {
            InQ = !InQ;
            continue;
        }
        if (*P != 0 && InQ) {
            continue;
        }
        if (*P != 0 && *P != L';' && *P != L'&' && *P != L'|') {
            continue;
        }
        Op = OP_NONE;
        Skip = 1;
        if (*P == L';') {
            Op = OP_SEMI;
        } else if (*P == L'&') {
            Op = (P[1] == L'&') ? OP_AND : OP_AMP;
            Skip = (P[1] == L'&') ? 2 : 1;
        } else if (*P == L'|') {
            Op = (P[1] == L'|') ? OP_OR : OP_PIPE;
            Skip = (P[1] == L'|') ? 2 : 1;
        }
        if (N >= OP_MAXSEG) {
            break;
        }
        Segs[N].Text = Start;
        Segs[N].Op = Op;
        N++;
        if (*P == 0) {
            break;
        }
        *P = 0;
        P += Skip - 1;
        Start = P + 1;
    }
    return N;
}

static BOOLEAN OpBlank(CHAR16 *T)
{
    for (; *T; T++) {
        if (*T != L' ' && *T != L'\t') {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN OpRun(OpSeg *S, INTN From, INTN To)
{
    CHAR16 InName[MAXP];
    CHAR16 OutName[MAXP];
    CHAR16 *T;
    OpBuf Buf;
    BOOLEAN Append;
    BOOLEAN ToFile;
    BOOLEAN Cap;
    BOOLEAN Failed = FALSE;
    UINTN L;
    INTN I;

    OpClearIn();
    for (I = From; I <= To; I++) {
        T = S[I].Text;
        if (!OpRedir(T, InName, OutName, &Append)) {
            Print(L"Syntax error: missing file name\n");
            OpClearIn();
            return TRUE;
        }
        L = StrLen(T);
        while (L > 0 && (T[L - 1] == L' ' || T[L - 1] == L'\t')) {
            T[--L] = 0;
        }
        while (*T == L' ' || *T == L'\t') {
            T++;
        }
        if (*T == 0) {
            Print(L"Syntax error: empty command\n");
            OpClearIn();
            return TRUE;
        }
        if (InName[0] != 0) {
            if (I != From) {
                Print(L"Syntax error: input redirection after a pipe\n");
                OpClearIn();
                return TRUE;
            }
            if (!OpLoad(InName)) {
                OpClearIn();
                return TRUE;
            }
        }
        ToFile = OutName[0] != 0;
        Cap = ToFile || I < To;
        ZeroMem(&Buf, sizeof(Buf));
        gFailed = FALSE;
        if (Cap) {
            OpCapBegin(&Buf);
        }
        if (!OpFilter(T)) {
            ProcessSimple(T);
        }
        if (Cap) {
            OpCapEnd();
        }
        Failed = gFailed;
        OpClearIn();
        if (ToFile) {
            if (!OpWriteFile(OutName, &Buf, Append)) {
                Failed = TRUE;
            }
            if (Buf.Data != NULL) {
                FreePool(Buf.Data);
            }
            if (I < To) {
                OpSetIn(NULL, 0);
            }
        } else if (Cap) {
            OpSetIn(Buf.Data, Buf.Len);
        }
    }
    OpClearIn();
    return Failed;
}

static VOID Process(CHAR16 *Line)
{
    OpSeg Segs[OP_MAXSEG];
    INTN N;
    INTN I = 0;
    INTN J;
    INTN Prev = OP_SEMI;
    BOOLEAN Failed = FALSE;
    BOOLEAN Run;

    N = OpSplit(Line, Segs);
    while (I < N && !gExit) {
        J = I;
        while (J < N - 1 && Segs[J].Op == OP_PIPE) {
            J++;
        }
        Run = TRUE;
        if (Prev == OP_AND && Failed) {
            Run = FALSE;
        }
        if (Prev == OP_OR && !Failed) {
            Run = FALSE;
        }
        if (Run && !(I == J && OpBlank(Segs[I].Text))) {
            Failed = OpRun(Segs, I, J);
        }
        Prev = Segs[J].Op;
        I = J + 1;
    }
}

static VOID PushHistory(CHAR16 *Line)
{
    UINTN I;

    if (Line[0] == 0) {
        return;
    }
    if (gHistCount > 0 && !StrCmp(gHistory[gHistCount - 1], Line)) {
        return;
    }
    if (gHistCount == HISTORY) {
        for (I = 1; I < HISTORY; I++) {
            StrCpy(gHistory[I - 1], gHistory[I]);
        }
        gHistCount--;
    }
    StrCpy(gHistory[gHistCount++], Line);
}

static VOID ReplaceLine(CHAR16 *Buf, UINTN *Len, CHAR16 *With)
{
    UINTN I;

    for (I = 0; I < *Len; I++) {
        Out(L"\b \b");
    }
    StrCpy(Buf, With);
    *Len = StrLen(Buf);
    Out(Buf);
}

static VOID ReadLine(CHAR16 *Buf, UINTN Max)
{
    UINTN Len = 0;
    INTN Hist = (INTN)gHistCount;
    EFI_INPUT_KEY K;
    CHAR16 One[2];

    Buf[0] = 0;
    for (;;) {
        K = GetKey();
        if (K.UnicodeChar == L'\r' || K.UnicodeChar == L'\n') {
            Out(L"\r\n");
            return;
        }
        if (K.UnicodeChar == 8) {
            if (Len > 0) {
                Len--;
                Buf[Len] = 0;
                Out(L"\b \b");
            }
            continue;
        }
        if (K.ScanCode == 1) {
            if (Hist > 0) {
                Hist--;
                ReplaceLine(Buf, &Len, gHistory[Hist]);
            }
            continue;
        }
        if (K.ScanCode == 2) {
            if (Hist + 1 < (INTN)gHistCount) {
                Hist++;
                ReplaceLine(Buf, &Len, gHistory[Hist]);
            } else if (Hist < (INTN)gHistCount) {
                Hist = (INTN)gHistCount;
                ReplaceLine(Buf, &Len, L"");
            }
            continue;
        }
        if (K.ScanCode == 0x17) {
            ReplaceLine(Buf, &Len, L"");
            continue;
        }
        if (K.UnicodeChar >= 32 && Len + 1 < Max) {
            Buf[Len++] = K.UnicodeChar;
            Buf[Len] = 0;
            One[0] = K.UnicodeChar;
            One[1] = 0;
            Out(One);
        }
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE *Loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    CHAR16 Line[MAXLINE];
    EFI_STATUS Status;

    InitializeLib(ImageHandle, SystemTable);
    gImage = ImageHandle;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&Loaded);
    if (EFI_ERROR(Status) || Loaded == NULL || Loaded->DeviceHandle == NULL) {
        Print(L"Command: cannot determine boot device: %r\n", Status);
        return EFI_LOAD_ERROR;
    }
    gDevice = Loaded->DeviceHandle;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, gDevice, &FileSystemProtocol, (VOID **)&Fs);
    if (EFI_ERROR(Status) || Fs == NULL) {
        Print(L"Command: boot device has no file system: %r\n", Status);
        return Status;
    }
    Status = uefi_call_wrapper(Fs->OpenVolume, 2, Fs, &gRoot);
    if (EFI_ERROR(Status)) {
        Print(L"Command: cannot open volume: %r\n", Status);
        return Status;
    }

    StrCpy(gCwd, L"\\");
    ResetConsole();
    Print(L"IzhOS Command Shell %s\n", SHELL_VERSION);
    Print(L"Type help for a list of commands.\n\n");

    while (!gExit) {
        Print(L"%s> ", gCwd);
        ReadLine(Line, MAXLINE);
        PushHistory(Line);
        Process(Line);
    }

    CloseFile(gRoot);
    return EFI_SUCCESS;
}

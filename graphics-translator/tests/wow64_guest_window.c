#include <windows.h>
#include <intrin.h>

static volatile LONG faults;

static LONG WINAPI handle_fault(EXCEPTION_POINTERS *exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2 ||
        exception->ExceptionRecord->ExceptionInformation[0] != 0 ||
        exception->ExceptionRecord->ExceptionInformation[1] != 0x12345000)
        return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    exception->ContextRecord->Eip += 2; /* movl (%eax), %eax */
    return EXCEPTION_CONTINUE_EXECUTION;
}

void probe_entry(void)
{
    static const char entered[] = "WOW64_WINDOW_ENTER\n";
    static const char image_prefix[] = "WOW64_WINDOW_IMAGE:";
    static const char passed[] = "WOW64_WINDOW_PASS: entry fs32 allocation read-write query guest-fault-address\n";
    char image_path[2 * MAX_PATH];
    DWORD image_length;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    DWORD written;
    MEMORY_BASIC_INFORMATION info;
    volatile ULONG *memory;
    void *handler;
    ULONG address = 0x12345000;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), entered, sizeof(entered) - 1, &written, NULL);
    dos = (IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x1000) ExitProcess(17);
    nt = (IMAGE_NT_HEADERS32 *)((BYTE *)dos + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) ExitProcess(18);
    for (image_length = 0; image_length < sizeof(image_prefix) - 1; image_length++)
        image_path[image_length] = image_prefix[image_length];
    image_length = GetModuleFileNameA(NULL, image_path + sizeof(image_prefix) - 1, sizeof(image_path) - sizeof(image_prefix));
    if (!image_length || image_length >= sizeof(image_path) - sizeof(image_prefix)) ExitProcess(19);
    image_length += sizeof(image_prefix) - 1;
    image_path[image_length++] = '\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), image_path, image_length, &written, NULL);
    if (__readfsdword(0x18) == 0 || __readfsdword(0x30) == 0) ExitProcess(10);
    memory = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) ExitProcess(11);
    memory[0] = 0x12345678;
    memory[0x3fff] = 0x89abcdef;
    if (memory[0] != 0x12345678 || memory[0x3fff] != 0x89abcdef) ExitProcess(12);
    if (VirtualQuery((const void *)memory, &info, sizeof(info)) != sizeof(info) ||
        info.BaseAddress != (void *)memory || info.AllocationBase != (void *)memory ||
        info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) ExitProcess(13);
    handler = AddVectoredExceptionHandler(1, handle_fault);
    if (!handler) ExitProcess(14);
    __asm__ volatile("movl (%%eax), %%eax" : "+a"(address) : : "memory");
    if (faults != 1) ExitProcess(15);
    if (!RemoveVectoredExceptionHandler(handler) || !VirtualFree((void *)memory, 0, MEM_RELEASE)) ExitProcess(16);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), passed, sizeof(passed) - 1, &written, NULL);
    ExitProcess(0);
}

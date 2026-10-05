#include <windows.h>
#include <winternl.h>

void probe_entry(void)
{
    typedef LONG (WINAPI *allocate_jit_fn)(void **, SIZE_T *);
    typedef BOOL (WINAPI *write_protect_fn)(BOOL);
    typedef LONG (WINAPI *query_process_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    static const char entered[] = "NATIVE_JIT_ENTER\n";
    static const char passed[] = "NATIVE_JIT_PASS: committed native allocation nested write scopes execution result=42 no-EC guest-window-disjoint\n";
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    allocate_jit_fn allocate = (allocate_jit_fn)GetProcAddress(ntdll, "__wine_allocate_native_jit");
    write_protect_fn protect = (write_protect_fn)GetProcAddress(ntdll, "__wine_jit_write_protect");
    query_process_fn query = (query_process_fn)GetProcAddress(ntdll, "NtQueryInformationProcess");
    /* PEB64.EcCodeBitMap offset from the pinned Wine winternl.h ABI. */
    void *const *ec_bitmap = (void *const *)((BYTE *)NtCurrentTeb()->ProcessEnvironmentBlock + 0x368);
    ULONG_PTR guest_base = 0;
    void *code = NULL, *bad = (void *)0x1234;
    SIZE_T size = 8, bad_size = 8;
    DWORD written;
    BOOL outer, inner;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), entered, sizeof(entered) - 1, &written, NULL);
    if (!allocate || !protect || !query || protect(2) != TRUE || *ec_bitmap) ExitProcess(10);
    if (query(GetCurrentProcess(), 1010, &guest_base, sizeof(guest_base), NULL) ||
        !guest_base || (guest_base & 0xffffffff)) ExitProcess(11);
    if (allocate(&bad, &bad_size) >= 0 || bad != (void *)0x1234 || bad_size != 8) ExitProcess(12);
    if (allocate(&code, &size) || !code || size < 8 ||
        ((ULONG_PTR)code >= guest_base && (ULONG_PTR)code - guest_base < 0x100000000ULL) || *ec_bitmap) ExitProcess(13);
    outer = protect(FALSE);
    inner = protect(FALSE);
    if (outer != TRUE || inner != FALSE) ExitProcess(14);
    ((ULONG *)code)[0] = 0x52800540; /* mov w0, #42 */
    ((ULONG *)code)[1] = 0xd65f03c0; /* ret */
    if (protect(inner) != FALSE || protect(outer) != FALSE || protect(2) != TRUE) ExitProcess(15);
    if (!FlushInstructionCache(GetCurrentProcess(), code, 8) || ((int (*)(void))code)() != 42) ExitProcess(16);
    if (!VirtualFree(code, 0, MEM_RELEASE)) ExitProcess(17);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), passed, sizeof(passed) - 1, &written, NULL);
    ExitProcess(0);
}

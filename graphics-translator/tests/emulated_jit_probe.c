#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    typedef void *(WINAPI *allocate_fn)(HANDLE, void *, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
    allocate_fn allocate = (allocate_fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
    const DWORD permissions[] = { PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READWRITE | PAGE_TARGETS_INVALID };
    const BYTE instructions[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 }; /* mov eax, 42; ret */

    if (!allocate) return 1;
    for (unsigned int i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++)
    {
        void *region = allocate(NULL, NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_NOACCESS, NULL, 0);
        void *page = region ? allocate(NULL, region, 262144, MEM_COMMIT, permissions[i], NULL, 0) : NULL;
        MEMORY_BASIC_INFORMATION info;
        if (!page || page != region)
        {
            printf("EMULATED_JIT_FAIL: protect=%lx error=%lu\n", permissions[i], GetLastError());
            if (region) VirtualFree(region, 0, MEM_RELEASE);
            return 2;
        }
        if (VirtualQuery(page, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & 0xff) != PAGE_EXECUTE_READWRITE) return 3;
        memcpy(page, instructions, sizeof(instructions));
        if (!FlushInstructionCache(GetCurrentProcess(), page, sizeof(instructions)) || ((int (*)(void))page)() != 42)
            return 4;
        if (!VirtualFree(region, 0, MEM_RELEASE)) return 5;
    }
    puts("EMULATED_JIT_PASS: reserved cage RWX commit CFG permissions execution result=42");
    return 0;
}

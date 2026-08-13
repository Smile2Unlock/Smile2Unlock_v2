#include <windows.h>
#include <stdio.h>
int main(void) {
    char *p = (char*)VirtualAlloc(NULL, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!p) { printf("alloc fail %lu\n", GetLastError()); return 1; }
    p[0] = 'X';
    DWORD old = 0;
    BOOL ok = VirtualProtect(p, 4096, PAGE_NOACCESS, &old);
    printf("protect->noaccess ok=%d err=%lu\n", ok, GetLastError());
    MEMORY_BASIC_INFORMATION mbi = {0};
    SIZE_T n = VirtualQuery(p, &mbi, sizeof(mbi));
    printf("query n=%zu protect=%lx\n", n, mbi.Protect);
    ok = VirtualProtect(p, 4096, PAGE_READWRITE, &old);
    printf("protect->rw ok=%d err=%lu\n", ok, GetLastError());
    p[0] = 'Y';
    printf("write ok: %c\n", p[0]);
    ok = VirtualLock(p, 4096);
    printf("lock ok=%d err=%lu\n", ok, GetLastError());
    BOOL u = VirtualUnlock(p, 4096);
    printf("unlock ok=%d err=%lu\n", u, GetLastError());
    VirtualFree(p, 0, MEM_RELEASE);
    printf("done\n");
    return 0;
}

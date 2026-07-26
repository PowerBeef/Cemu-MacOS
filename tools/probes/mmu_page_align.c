// Demonstrates the 16 KB page bug in MemMapper and the fix.
//
// Faithful to the original code, which was asymmetric:
//   AllocateMemory(fromReservation) rounded the BASE down but never extended the size
//   FreeMemory(fromReservation)     did no alignment fix-up at all, and ignored the result
//
// On a 4 KB-page machine both work. On Apple silicon (16 KB pages) the decommit of a
// range whose base is not 16 KB-aligned returns EINVAL, is discarded, and the guest
// range stays writable across a title unload.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t gPageSize;

static void roundBaseOnly(void** base, size_t* size)   // original AllocateMemory
{
    uintptr_t s = (uintptr_t)*base;
    *base = (void*)(s & ~(uintptr_t)(gPageSize - 1));
    (void)size;                                        // size deliberately left alone
}
static void roundBoth(void** base, size_t* size)       // fixed AllocateMemory / FreeMemory
{
    uintptr_t s = (uintptr_t)*base, e = s + *size;
    uintptr_t as = s & ~(uintptr_t)(gPageSize - 1);
    uintptr_t ae = (e + gPageSize - 1) & ~(uintptr_t)(gPageSize - 1);
    *base = (void*)as; *size = (size_t)(ae - as);
}

static void cycle(const char* label, uint8_t* memBase, uint32_t gBase, uint32_t gSize, int fixed)
{
    void* cp = memBase + gBase; size_t csz = gSize;
    if (fixed) roundBoth(&cp, &csz); else roundBaseOnly(&cp, &csz);
    int commitErr = mprotect(cp, csz, PROT_READ | PROT_WRITE) == 0 ? 0 : errno;

    void* dp = memBase + gBase; size_t dsz = gSize;
    if (fixed) roundBoth(&dp, &dsz);                   // original FreeMemory rounded nothing
    int freeErr = mprotect(dp, dsz, PROT_NONE) == 0 ? 0 : errno;

    printf("  %-24s commit %-22s decommit %s\n", label,
           commitErr ? strerror(commitErr) : "ok",
           freeErr ? strerror(freeErr) : "ok");
    if (freeErr)
        printf("  %-24s -> range stays writable across title unload\n", "");
}

int main(void)
{
    gPageSize = (size_t)getpagesize();
    printf("host page size: %zu bytes\n\n", gPageSize);

    uint8_t* memBase = mmap(NULL, 0x100000000ULL, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memBase == MAP_FAILED) { perror("reserve 4GB"); return 1; }

    printf("HIGHMEM as originally declared  (base 0xFFFFF000, size 0x1000):\n");
    cycle("original MemMapper", memBase, 0xFFFFF000u, 0x1000u, 0);
    cycle("fixed MemMapper",    memBase, 0xFFFFF000u, 0x1000u, 1);

    printf("\nHIGHMEM as now declared         (base 0xFFFFC000, size 0x4000):\n");
    cycle("original MemMapper", memBase, 0xFFFFC000u, 0x4000u, 0);
    cycle("fixed MemMapper",    memBase, 0xFFFFC000u, 0x4000u, 1);

    printf("\nCORE0_LC as originally declared (base 0xFFC00000, size 0x5000):\n");
    cycle("original MemMapper", memBase, 0xFFC00000u, 0x5000u, 0);
    cycle("fixed MemMapper",    memBase, 0xFFC00000u, 0x5000u, 1);
    return 0;
}

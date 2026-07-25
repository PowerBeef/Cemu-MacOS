// Faithful reproduction of xbyak_aarch64's MmapAllocator::alloc + CodeArray::protect:
//   mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT)   <-- note: no PROT_EXEC
//   memcpy(code)
//   mprotect(PROT_READ|PROT_EXEC)
//   call
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

typedef int (*fn_t)(void);
static sigjmp_buf g_jmp;
static volatile const char* g_stage = "?";
static void handler(int sig){ printf("  !! signal %d during stage: %s\n", sig, g_stage); siglongjmp(g_jmp,1); }
static const unsigned int CODE[] = { 0x52800540u, 0xd65f03c0u };  // mov w0,#42 ; ret

static void run(const char* name, int jitwp, int flush)
{
    printf("[%s] jit_write_protect_np=%d icache_flush=%d\n", name, jitwp, flush);
    size_t sz = (size_t)getpagesize();
    g_stage="mmap";
    void* p = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT, -1, 0);
    if (p==MAP_FAILED){ printf("  mmap failed: %s\n\n", strerror(errno)); return; }
    if (sigsetjmp(g_jmp,1)!=0){ printf("  -> FAULTED\n\n"); return; }
    g_stage="write";
    if (jitwp) pthread_jit_write_protect_np(0);
    memcpy(p, CODE, sizeof(CODE));
    if (jitwp) pthread_jit_write_protect_np(1);
    g_stage="mprotect RE";
    if (mprotect(p, sz, PROT_READ|PROT_EXEC)!=0) printf("  mprotect(RE) failed: %s\n", strerror(errno));
    if (flush) sys_icache_invalidate(p, sizeof(CODE));
    g_stage="execute";
    int v = ((fn_t)p)();
    printf("  -> returned %d (%s)\n\n", v, v==42?"OK":"WRONG");
}

int main(void){
    signal(SIGBUS,handler); signal(SIGSEGV,handler); signal(SIGILL,handler);
    run("EXACT xbyak_aarch64 pattern", 0, 0);
    run("xbyak + icache flush", 0, 1);
    run("xbyak + jit_write_protect + flush", 1, 1);
    return 0;
}

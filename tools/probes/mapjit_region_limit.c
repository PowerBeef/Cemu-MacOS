// R1 probe: how many MAP_JIT regions can one process create?
//
// The recompiler currently constructs a fresh xbyak allocator + CodeGenerator per
// recompiled PPC function, i.e. one mmap per function. Apple's docs state that under
// the hardened runtime with com.apple.security.cs.allow-jit an app "can only create
// one memory region with the MAP_JIT flag set". If that is enforced, the current
// model breaks the moment the app is signed, and the JitCodeArena work becomes a
// blocker rather than an optimization.
//
// This also exercises the full write->execute cycle on the first region so we know
// pthread_jit_write_protect_np + sys_icache_invalidate actually work as expected.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

#define REGION_SIZE (64u << 10)  // 64 KB, closer to a per-function JIT buffer
#define MAX_REGIONS 4000

typedef int (*fn_t)(void);

int main(void)
{
	void* regions[MAX_REGIONS];
	int ok = 0;

	for (int i = 0; i < MAX_REGIONS; i++)
	{
		void* p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (p == MAP_FAILED)
		{
			printf("MAP_JIT region %d FAILED: %s (errno %d)\n", i, strerror(errno), errno);
			break;
		}
		regions[ok++] = p;
	}
	printf("MAP_JIT regions successfully created: %d (of %d attempted)\n", ok, MAX_REGIONS);

	if (ok == 0)
	{
		printf("RESULT: no MAP_JIT region at all -- missing entitlement or not hardened.\n");
		return 2;
	}

	// Exercise write -> execute on the first region.
	// mov w0, #0x2A ; ret   (returns 42)
	const unsigned int code[] = { 0x52800540u, 0xd65f03c0u };

	pthread_jit_write_protect_np(0);              // W
	memcpy(regions[0], code, sizeof(code));
	pthread_jit_write_protect_np(1);              // X
	sys_icache_invalidate(regions[0], sizeof(code));

	fn_t f = (fn_t)regions[0];
	int v = f();
	printf("executed JIT-written code, returned %d (expected 42) -> %s\n",
	       v, v == 42 ? "OK" : "MISMATCH");

	if (ok == 1)
		printf("RESULT: ONE-REGION LIMIT ENFORCED. Per-function mmap cannot work; "
		       "a single arena is mandatory.\n");
	else
		printf("RESULT: %d regions allowed -- the one-region limit is NOT enforced here.\n", ok);

	return v == 42 ? 0 : 3;
}

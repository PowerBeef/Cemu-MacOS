// How much does a guest thread switch actually cost?
//
// BotW does 3,143 of them per frame in the open world, 79% of which are voluntary
// OSYieldThread -- BotW polling, faithfully emulated, because Cafe OS scheduling is
// cooperative. On console a yield is a cheap scheduler call. Here every one is a
// ucontext fiber switch (src/util/Fiber/FiberUnix.cpp), and swapcontext on Darwin/arm64
// calls sigprocmask on both save and restore.
//
// docs/porting/02-cpu-jit-memory.md 4.2 estimates ~600-700 ns/switch and predicts ~15 ns
// for a hand-written AArch64 switch. That estimate has never been measured, and it CANNOT
// be measured in-process with a scope timer: Fiber::Switch does not return until the fiber
// is resumed, so timing around it captures descheduled time, not switch cost -- the trap
// that once produced 197 ms of "busy" inside a 49.9 ms frame.
//
// A ping-pong microbenchmark has no such problem. Two fibers hand control back and forth
// N times on one thread with nothing else running, so wall-clock / (2N) is the switch cost
// directly. Multiply by the already-measured switch count for the frame-level figure.
//
// Build and run:
//   clang -O2 -o fiber_switch_cost fiber_switch_cost.c && ./fiber_switch_cost
//
// swapcontext is deprecated on macOS (and the header hides it under _XOPEN_SOURCE), which
// is a fact about the API this emulator depends on today, not a reason not to measure it.

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ucontext.h>
#include <mach/mach_time.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define STACK_SIZE (2 * 1024 * 1024)   // matches FiberUnix.cpp:11
#define ITERATIONS 1000000

// ---------------------------------------------------------------- ucontext ping-pong

static ucontext_t g_ctxMain, g_ctxWorker;
static volatile uint64_t g_counter;

static void worker_ucontext(void)
{
	for (;;)
	{
		g_counter++;
		swapcontext(&g_ctxWorker, &g_ctxMain);
	}
}

static double bench_ucontext(uint64_t iterations)
{
	void* stack = malloc(STACK_SIZE);            // FiberUnix.cpp:12 -- malloc, no guard page
	getcontext(&g_ctxWorker);
	g_ctxWorker.uc_stack.ss_sp = stack;
	g_ctxWorker.uc_stack.ss_size = STACK_SIZE;
	g_ctxWorker.uc_link = &g_ctxMain;
	makecontext(&g_ctxWorker, (void (*)(void))worker_ucontext, 0);

	// warm up: first switch faults in the stack and the ucontext pages
	for (int i = 0; i < 1000; i++)
		swapcontext(&g_ctxMain, &g_ctxWorker);

	uint64_t t0 = mach_absolute_time();
	for (uint64_t i = 0; i < iterations; i++)
		swapcontext(&g_ctxMain, &g_ctxWorker);
	uint64_t t1 = mach_absolute_time();

	free(stack);
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	// two context switches per iteration: out to the worker and back
	return (double)(t1 - t0) * tb.numer / tb.denom / (double)(iterations * 2);
}

// ------------------------------------------------- minimal hand-written AArch64 switch
//
// The 176-byte frame from docs/porting/02-cpu-jit-memory.md 4.2: x19-x28, x29/x30, sp,
// d8-d15 (low halves only -- the upper halves of v8-v15 are caller-saved), and FPCR.
// x18 is untouched: reserved by the Darwin kernel.
//
// This is a *measurement* of what the replacement would cost, not the replacement itself.
// A shippable version needs CFI (without it lldb backtraces and the Instruments sampler
// produce junk from inside a guest fiber, silently invalidating the profiling harness) and
// mmap'd stacks with 16 KB guard pages. Neither changes the cycle count below.

struct fiber_frame { uint64_t s[22]; };   // 11 pairs, 16-byte aligned

__attribute__((naked, noinline))
static void fiber_swap(struct fiber_frame* save, struct fiber_frame* restore)
{
	__asm__ volatile(
		"stp x19, x20, [x0, #0x00]\n"
		"stp x21, x22, [x0, #0x10]\n"
		"stp x23, x24, [x0, #0x20]\n"
		"stp x25, x26, [x0, #0x30]\n"
		"stp x27, x28, [x0, #0x40]\n"
		"stp x29, x30, [x0, #0x50]\n"
		"stp d8,  d9,  [x0, #0x60]\n"
		"stp d10, d11, [x0, #0x70]\n"
		"stp d12, d13, [x0, #0x80]\n"
		"stp d14, d15, [x0, #0x90]\n"
		"mov x9, sp\n"
		"mrs x10, fpcr\n"
		"stp x9, x10, [x0, #0xA0]\n"

		"ldp x19, x20, [x1, #0x00]\n"
		"ldp x21, x22, [x1, #0x10]\n"
		"ldp x23, x24, [x1, #0x20]\n"
		"ldp x25, x26, [x1, #0x30]\n"
		"ldp x27, x28, [x1, #0x40]\n"
		"ldp x29, x30, [x1, #0x50]\n"
		"ldp d8,  d9,  [x1, #0x60]\n"
		"ldp d10, d11, [x1, #0x70]\n"
		"ldp d12, d13, [x1, #0x80]\n"
		"ldp d14, d15, [x1, #0x90]\n"
		"ldp x9, x10, [x1, #0xA0]\n"
		"mov sp, x9\n"
		"msr fpcr, x10\n"
		"ret\n");
}

static struct fiber_frame g_frameMain, g_frameWorker;

static void worker_asm(void)
{
	for (;;)
	{
		g_counter++;
		fiber_swap(&g_frameWorker, &g_frameMain);
	}
}

static double bench_asm(uint64_t iterations)
{
	void* stack = malloc(STACK_SIZE);
	// Build the initial frame by hand: land in worker_asm with a 16-aligned sp. Real fibers
	// need a trampoline here to pass an argument and trap on return; the benchmark does not.
	uint64_t stackTop = ((uint64_t)stack + STACK_SIZE) & ~15ull;
	memset(&g_frameWorker, 0, sizeof(g_frameWorker));
	g_frameWorker.s[11] = (uint64_t)&worker_asm;   // x30 (LR) -> entry
	g_frameWorker.s[20] = stackTop;                // sp
	__asm__ volatile("mrs %0, fpcr" : "=r"(g_frameWorker.s[21]));

	for (int i = 0; i < 1000; i++)
		fiber_swap(&g_frameMain, &g_frameWorker);

	uint64_t t0 = mach_absolute_time();
	for (uint64_t i = 0; i < iterations; i++)
		fiber_swap(&g_frameMain, &g_frameWorker);
	uint64_t t1 = mach_absolute_time();

	free(stack);
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (double)(t1 - t0) * tb.numer / tb.denom / (double)(iterations * 2);
}

int main(void)
{
	// BotW, Korok Forest, from the telemetry harness. Used only to scale the result.
	const double switchesPerFrame = 3143.0;
	const double frameMs = 49.90;

	printf("ping-pong, %d iterations (%d switches each way)\n\n", ITERATIONS, ITERATIONS);

	double nsUcontext = 0.0, nsAsm = 0.0;
	// Median of 5 runs each, interleaved, so thermal drift hits both variants equally
	// rather than whichever ran second.
	double u[5], a[5];
	for (int r = 0; r < 5; r++)
	{
		u[r] = bench_ucontext(ITERATIONS);
		a[r] = bench_asm(ITERATIONS);
	}
	for (int i = 0; i < 5; i++)
		for (int j = i + 1; j < 5; j++)
		{
			if (u[j] < u[i]) { double t = u[i]; u[i] = u[j]; u[j] = t; }
			if (a[j] < a[i]) { double t = a[i]; a[i] = a[j]; a[j] = t; }
		}
	nsUcontext = u[2];
	nsAsm = a[2];

	printf("  swapcontext (what FiberUnix.cpp uses today)\n");
	printf("    median   %8.2f ns/switch      range %.2f - %.2f\n", nsUcontext, u[0], u[4]);
	printf("  hand-written AArch64 (176-byte frame, no syscall)\n");
	printf("    median   %8.2f ns/switch      range %.2f - %.2f\n", nsAsm, a[0], a[4]);
	printf("    speedup  %8.2fx\n\n", nsUcontext / nsAsm);

	double curMs = nsUcontext * switchesPerFrame / 1e6;
	double newMs = nsAsm * switchesPerFrame / 1e6;
	printf("  at %.0f guest thread switches/frame (BotW, Korok Forest):\n", switchesPerFrame);
	printf("    today          %6.3f ms/frame  = %.2f%% of a %.2f ms frame\n",
		   curMs, curMs / frameMs * 100.0, frameMs);
	printf("    hand-written   %6.3f ms/frame  = %.2f%%\n", newMs, newMs / frameMs * 100.0);
	printf("    saving         %6.3f ms/frame  = %.2f%%\n",
		   curMs - newMs, (curMs - newMs) / frameMs * 100.0);
	return 0;
}

// Does the ldxr/wfe park in TCLGPUWaitForRBData actually park?
//
// src/Cafe/OS/libs/TCL/TCL.cpp documents "~1269 ns" per idle pass, measured when the park
// was written (commit 612d064). Measured inside BotW the same loop takes 143 ns/pass --
// nine times shorter, i.e. the park is not parking and the loop has degraded back to a spin
// with a sched_yield in it.
//
// The suspected reason is that the original figure was taken in isolation. ARM's WFE returns
// immediately if the CPU's *event register* is already set, and the event register is set by
// a great deal more than a store to the monitored address: any SEV, any exclusive-monitor
// clear, and -- critically -- interrupts. TesseraEmu runs 40+ threads on 8 cores, so timer
// and IPI interrupts arrive constantly.
//
// This probe measures wfe latency with a varying number of busy sibling threads. If the
// hypothesis holds, latency collapses as soon as the machine is not idle.
//
//   clang -O2 -o wfe_under_load wfe_under_load.c -lpthread && ./wfe_under_load

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <mach/mach_time.h>

static _Atomic uint32_t g_watched;      // stands in for tclRingBufferA_writeIndex
static _Atomic int      g_stop;
static _Atomic uint64_t g_spinSink;

static double g_toNs;

static inline uint64_t now(void)
{
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

// Sibling threads that do ordinary work -- no stores to the watched address, no SEV. If wfe
// latency drops when these run, it is the machine being busy that wakes it, not the producer.
static void* busy(void* arg)
{
    (void)arg;
    uint64_t acc = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed))
    {
        for (int i = 0; i < 4096; i++)
            acc += i * 2654435761u;
        // A syscall now and then, like a real thread would. sched_yield is exactly what the
        // command-processor loop does 2.8 million times a second.
        sched_yield();
    }
    atomic_fetch_add_explicit(&g_spinSink, acc, memory_order_relaxed);
    return NULL;
}

// One pass of exactly what TCLGPUWaitForRBData does when the ring is empty.
static uint64_t one_park_pass(void)
{
    uint32_t* addr = (uint32_t*)&g_watched;
    uint32_t observed;
    uint64_t t0 = now();
    __asm__ volatile("ldxr %w0, [%1]" : "=r"(observed) : "r"(addr) : "memory");
    __asm__ volatile("wfe" ::: "memory");
    uint64_t t1 = now();
    return t1 - t0;
}

static void measure(int nBusy, uint64_t iterations)
{
    pthread_t th[16];
    atomic_store(&g_stop, 0);
    for (int i = 0; i < nBusy; i++)
        pthread_create(&th[i], NULL, busy, NULL);
    if (nBusy)
        usleep(50 * 1000); // let them get going

    for (int i = 0; i < 1000; i++) one_park_pass();   // warm

    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    for (uint64_t i = 0; i < iterations; i++)
    {
        uint64_t d = one_park_pass();
        total += d;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }

    atomic_store(&g_stop, 1);
    for (int i = 0; i < nBusy; i++)
        pthread_join(th[i], NULL);

    double avgNs = (double)total / iterations * g_toNs;
    printf("  %2d busy sibling thread(s):  mean %8.1f ns   min %6.0f   max %8.0f\n",
           nBusy, avgNs, mn * g_toNs, mx * g_toNs);
}

int main(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    g_toNs = 1e9 / (double)f;

    printf("cntfrq_el0 = %llu Hz  (%.3f ns/tick)\n", (unsigned long long)f, g_toNs);
    printf("\nOne ldxr+wfe pass on an address nobody writes -- so every return is the WFE\n"
           "giving up on its own, not a wakeup:\n\n");

    measure(0, 20000);
    measure(1, 20000);
    measure(3, 20000);
    measure(7, 20000);

    printf("\nFor reference, TCL.cpp claims ~1269 ns/pass and BotW measures 143 ns/pass.\n");
    return 0;
}

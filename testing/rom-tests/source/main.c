/*
 * Cafe OS / GX2 assertion ROM.
 *
 * Unlike testing/cpu-tests (one monolithic routine returning a failure count), this is a
 * suite of independently named assertions, each reporting its own verdict:
 *
 *     TEST <name> PASS
 *     TEST <name> FAIL expected=<x> got=<y>
 *     TEST <name> SKIP reason=<why>
 *
 * That shape is what lets the suite grow. A new test is a function and a line in the
 * table, and the runner picks it up with no changes -- which is the whole point of having
 * a harness rather than a script per suite.
 *
 * What belongs here: things that are cheap to assert, would silently corrupt if wrong, and
 * need no hardware to check. Things that need a reference implementation or real silicon
 * belong in cpu-tests (ppc750cl.s) or in a differential harness, not here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/enum.h>
#include <gx2/surface.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

static int s_pass, s_fail, s_skip;

static void pass(const char *name)
{
   s_pass++;
   WHBLogPrintf("TEST %s PASS", name);
}

static void fail(const char *name, const char *expected, const char *got)
{
   s_fail++;
   WHBLogPrintf("TEST %s FAIL expected=%s got=%s", name, expected, got);
}

static void skip(const char *name, const char *why)
{
   s_skip++;
   WHBLogPrintf("TEST %s SKIP reason=%s", name, why);
}

static void check(const char *name, int ok, const char *expected, const char *got)
{
   if (ok) pass(name); else fail(name, expected, got);
}

/* ---------------------------------------------------------------- coreinit: memory */

/* MEMAllocFromDefaultHeapEx must honour the requested alignment. Getting this wrong is
 * invisible until something needs a cache-line- or page-aligned buffer -- which is exactly
 * what GX2 surfaces need. */
static void test_mem_alignment(void)
{
   static const uint32_t aligns[] = {4, 32, 64, 128, 256, 4096};
   char got[64];
   for (unsigned i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
      void *p = MEMAllocFromDefaultHeapEx(1024, aligns[i]);
      char name[48];
      snprintf(name, sizeof(name), "mem_align_%u", (unsigned)aligns[i]);
      if (!p) { fail(name, "non-null", "NULL"); continue; }
      snprintf(got, sizeof(got), "0x%08x", (unsigned)(uintptr_t)p);
      check(name, ((uintptr_t)p % aligns[i]) == 0, "aligned", got);
      MEMFreeToDefaultHeap(p);
   }
}

/* A freshly allocated block must be writable across its whole length. A too-small
 * allocation shows up here rather than as heap corruption three tests later. */
static void test_mem_writable(void)
{
   const uint32_t size = 64 * 1024;
   uint8_t *p = MEMAllocFromDefaultHeapEx(size, 64);
   if (!p) { fail("mem_writable", "non-null", "NULL"); return; }
   memset(p, 0xA5, size);
   int ok = 1;
   for (uint32_t i = 0; i < size; i += 997)   /* prime stride: catches partial writes */
      if (p[i] != 0xA5) { ok = 0; break; }
   check("mem_writable", ok, "0xA5 throughout", ok ? "0xA5" : "mismatch");
   MEMFreeToDefaultHeap(p);
}

/* OSBlockMove is used constantly by the guest and appears in the HLE-call histogram as one
 * of the hottest calls in BotW, so a wrong implementation would be pervasive. */
static void test_osblockmove(void)
{
   uint8_t src[256], dst[256];
   for (int i = 0; i < 256; i++) { src[i] = (uint8_t)i; dst[i] = 0; }
   OSBlockMove(dst, src, sizeof(src), TRUE);
   check("osblockmove", memcmp(src, dst, sizeof(src)) == 0, "src==dst", "differs");
}

/* ---------------------------------------------------------------- coreinit: time */

/* OSGetSystemTime must not go backwards. The emulator drives guest time from a software
 * timer, and a non-monotonic clock breaks frame pacing in ways that look like stutter. */
static void test_time_monotonic(void)
{
   OSTime prev = OSGetSystemTime();
   int ok = 1;
   for (int i = 0; i < 2000; i++) {
      OSTime now = OSGetSystemTime();
      if (now < prev) { ok = 0; break; }
      prev = now;
   }
   check("time_monotonic", ok, "non-decreasing", ok ? "non-decreasing" : "went backwards");
}

/* And it must actually advance -- a stopped clock is monotonic too. */
static void test_time_advances(void)
{
   OSTime start = OSGetSystemTime();
   for (volatile int i = 0; i < 2000000; i++) { }
   check("time_advances", OSGetSystemTime() > start, ">start", "did not advance");
}

/* ---------------------------------------------------------------- coreinit: threads */

/* Four out of five guest context switches in BotW are voluntary OSYieldThread calls, so a
 * yield that never returns would be catastrophic and a yield that does nothing is invisible. */
static void test_yield_returns(void)
{
   for (int i = 0; i < 1000; i++)
      OSYieldThread();
   pass("yield_returns");
}

static void test_core_id(void)
{
   uint32_t core = OSGetCoreId();
   char got[32];
   snprintf(got, sizeof(got), "%u", (unsigned)core);
   check("core_id_in_range", core < 3, "0..2", got);
}

/* ---------------------------------------------------------------- GX2: surfaces */

/* GX2CalcSurfaceSizeAndAlignment is pure, checkable, and drives every texture allocation.
 * A wrong size or alignment here is silent memory corruption, which is the worst class of
 * bug this emulator can have. */
static void test_gx2_surface(void)
{
   GX2Surface s;
   memset(&s, 0, sizeof(s));
   s.use = GX2_SURFACE_USE_TEXTURE;
   s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s.width = 256; s.height = 256; s.depth = 1; s.mipLevels = 1;
   s.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s.aa = GX2_AA_MODE1X;
   s.tileMode = GX2_TILE_MODE_DEFAULT;
   GX2CalcSurfaceSizeAndAlignment(&s);

   char got[64];
   snprintf(got, sizeof(got), "%u", (unsigned)s.imageSize);
   /* 256x256 at 4 bytes/texel is 256 KB before tiling; tiling may pad, never shrink. */
   check("gx2_surface_size_min", s.imageSize >= 256u * 256u * 4u, ">=262144", got);

   snprintf(got, sizeof(got), "%u", (unsigned)s.alignment);
   check("gx2_surface_align_pow2",
         s.alignment != 0 && (s.alignment & (s.alignment - 1)) == 0, "power of two", got);

   snprintf(got, sizeof(got), "%u", (unsigned)s.tileMode);
   /* DEFAULT must be resolved to a concrete mode; leaving it as DEFAULT means the addrlib
    * never ran and every downstream offset calculation is against the wrong layout. */
   check("gx2_tilemode_resolved", s.tileMode != GX2_TILE_MODE_DEFAULT, "!=DEFAULT", got);
}

/* Mip chains are where tiling maths most often goes wrong, because each level re-tiles. */
static void test_gx2_mipmapped(void)
{
   GX2Surface s;
   memset(&s, 0, sizeof(s));
   s.use = GX2_SURFACE_USE_TEXTURE;
   s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s.width = 512; s.height = 512; s.depth = 1; s.mipLevels = 10;
   s.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s.aa = GX2_AA_MODE1X;
   s.tileMode = GX2_TILE_MODE_DEFAULT;
   GX2CalcSurfaceSizeAndAlignment(&s);

   char got[64];
   snprintf(got, sizeof(got), "%u", (unsigned)s.mipmapSize);
   check("gx2_mipmap_size_nonzero", s.mipmapSize > 0, ">0", got);
   snprintf(got, sizeof(got), "%u", (unsigned)s.imageSize);
   check("gx2_mip_base_ge_flat", s.imageSize >= 512u * 512u * 4u, ">=1048576", got);
}

/* ---------------------------------------------------------------- runner */

struct test { const char *name; void (*fn)(void); };

static const struct test TESTS[] = {
   {"mem_alignment",   test_mem_alignment},
   {"mem_writable",    test_mem_writable},
   {"osblockmove",     test_osblockmove},
   {"time_monotonic",  test_time_monotonic},
   {"time_advances",   test_time_advances},
   {"yield_returns",   test_yield_returns},
   {"core_id",         test_core_id},
   {"gx2_surface",     test_gx2_surface},
   {"gx2_mipmapped",   test_gx2_mipmapped},
};

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogCafeInit();

   WHBLogPrintf("TESSERA-ROMTEST begin suite=cafeos groups=%u",
                (unsigned)(sizeof(TESTS) / sizeof(TESTS[0])));

   for (unsigned i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++)
      TESTS[i].fn();

   /* The per-test lines above are the result; this is a cross-check so a truncated log
    * cannot look like a clean run. */
   WHBLogPrintf("TESSERA-ROMTEST end pass=%d fail=%d skip=%d", s_pass, s_fail, s_skip);

   WHBLogCafeDeinit();
   WHBProcShutdown();
   return s_fail == 0 ? 0 : 1;
}

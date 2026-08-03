/*
 * Wii U homebrew runner for Andrew Church's PowerPC 750CL test suite.
 *
 * Why a .rpx rather than a host-side harness: the suite is guest PowerPC code,
 * so it has to execute on the emulated Espresso. Shipping it as ordinary
 * homebrew means it runs through the same path a real title does -- RPX load,
 * Cafe OS init, the recompiler -- with no new emulator plumbing at all. It also
 * needs no keys, no console and no game dump.
 *
 * Results go to OSReport via WHBLogCafeInit, so run the emulator with
 * --forward-console-logging and the output lands on stdout.
 *
 * The suite itself (ppc750cl.s) is unmodified and public domain: "No copyright
 * is claimed on this file." It was validated by its author against a real
 * Espresso processor, which is why it is worth trusting as an oracle.
 */

#include <stdint.h>
#include <string.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

/* Defined by ppc750cl_entry.S. R3=0, R4=scratch, R5=failure buffer, F1=1.0. */
extern int ppc750cl_test(int zero, void *scratch, void *failures, double one);

/* The suite's documented buffer requirements. */
#define SCRATCH_SIZE  (32u * 1024u)   /* exactly 32k, pre-cleared, cache-aligned */
#define FAILURES_SIZE (64u * 1024u)   /* "at least 64k to avoid overruns"        */
#define CACHE_ALIGN   128u            /* Espresso L1 line is 32B; over-align     */

#define RECORD_WORDS  8u              /* each failure record is 8 words / 32 bytes */

/*
 * Word 0 is the failing instruction word, word 1 its address, words 2-5 are
 * auxiliary data whose meaning depends on which checker caught it, words 6-7
 * are unused except by the frsqrte reciprocal-table tests. We print all eight
 * raw rather than trying to interpret them: the aux layout varies per
 * instruction, and a wrong interpretation here would be worse than none.
 * Decoding belongs in the host-side report script, which can be corrected
 * without rebuilding a ROM.
 */
static void print_record(uint32_t index, const uint32_t *r)
{
	WHBLogPrintf("FAIL %u insn=%08X addr=%08X aux=%08X,%08X,%08X,%08X extra=%08X,%08X",
	             (unsigned)index, (unsigned)r[0], (unsigned)r[1], (unsigned)r[2],
	             (unsigned)r[3], (unsigned)r[4], (unsigned)r[5],
	             (unsigned)r[6], (unsigned)r[7]);
}

int main(int argc, char **argv)
{
	WHBProcInit();
	WHBLogCafeInit();

	WHBLogPrintf("TESSERA-CPUTEST begin");
	WHBLogPrintf("suite=ppc750cl.s source=achurch.org licence=public-domain");

	void *scratch  = MEMAllocFromDefaultHeapEx(SCRATCH_SIZE, CACHE_ALIGN);
	void *failures = MEMAllocFromDefaultHeapEx(FAILURES_SIZE, CACHE_ALIGN);

	if (!scratch || !failures) {
		WHBLogPrintf("TESSERA-CPUTEST RESULT=ERROR reason=allocation-failed");
		WHBLogPrintf("TESSERA-CPUTEST end");
		goto out;
	}

	/* Both buffers must be pre-cleared; the suite relies on it. */
	memset(scratch, 0, SCRATCH_SIZE);
	memset(failures, 0, FAILURES_SIZE);

	WHBLogPrintf("scratch=%p failures=%p", scratch, failures);

	int rc = ppc750cl_test(0, scratch, failures, 1.0);

	if (rc < 0) {
		/*
		 * Negative means the suite could not bootstrap itself, i.e. one of the
		 * instructions it assumes correct (beq/bne cr0, bl, blr, fcmpu, mflr,
		 * mtlr) is broken. That is a far more serious result than any count of
		 * ordinary failures, so it gets its own status.
		 */
		WHBLogPrintf("TESSERA-CPUTEST RESULT=BOOTSTRAP-FAILED rc=%d", rc);
	} else if (rc == 0) {
		WHBLogPrintf("TESSERA-CPUTEST RESULT=PASS failures=0");
	} else {
		WHBLogPrintf("TESSERA-CPUTEST RESULT=FAIL failures=%d", rc);

		uint32_t max = (uint32_t)rc;
		if (max > FAILURES_SIZE / (RECORD_WORDS * sizeof(uint32_t))) {
			/* Report the truncation rather than reading past the buffer. */
			max = FAILURES_SIZE / (RECORD_WORDS * sizeof(uint32_t));
			WHBLogPrintf("TESSERA-CPUTEST NOTE truncated_to=%u", (unsigned)max);
		}

		const uint32_t *records = (const uint32_t *)failures;
		for (uint32_t i = 0; i < max; i++)
			print_record(i, records + (i * RECORD_WORDS));
	}

	WHBLogPrintf("TESSERA-CPUTEST end");

out:
	if (scratch)  MEMFreeToDefaultHeap(scratch);
	if (failures) MEMFreeToDefaultHeap(failures);

	WHBLogCafeDeinit();
	WHBProcShutdown();
	return 0;
}

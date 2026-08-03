/*
 * Symbol wrapper for ppc750cl.s.
 *
 * The test file deliberately defines no entry symbol -- its own header says:
 *
 *     "Note that this file does not itself define a symbol for the beginning
 *      of the test routine.  You can edit this file to insert such a symbol,
 *      or include this file in another file immediately after a symbol
 *      definition."
 *
 * So this is the "immediately after a symbol definition" option, which keeps
 * ppc750cl.s byte-identical to what achurch.org publishes. Do not edit that
 * file; if it needs to change, change it here.
 */

	.section .text
	.align 2
	.globl  ppc750cl_test
	.type   ppc750cl_test, @function

ppc750cl_test:
	.include "ppc750cl.s"

	.size   ppc750cl_test, . - ppc750cl_test

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include <arm_acle.h>
#include <cmath>

// Gekko paired single math

void PPCInterpreter_PS_ADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	// Single-domain: HUGE+HUGE → Inf + OX (double fadd stays finite).
	const double r0 = ppc_fadds(hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fadds(hCPU->fpr[frA].fp1, hCPU->fpr[frB].fp1);
	ppc_ps_fma_note_suppress();
	// Pack before commit so VE leave-prev is not re-quantized.
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	// Would-be ps0 for FPRF when only ps1 VE-aborts.
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_SUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_fsubs(hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fsubs(hCPU->fpr[frA].fp1, hCPU->fpr[frB].fp1);
	ppc_ps_fma_note_suppress();
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MUL(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Raw frC; 25-bit + ldexp product inside ppc_fmuls.
	// Whole-register VE: invalid on either lane suppresses both writes.
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_fmuls(hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fmuls(hCPU->fpr[frA].fp1, hCPU->fpr[frC].fp1);
	ppc_ps_fma_note_suppress();
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_DIV(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Double-specials then single pack; whole-register VE/ZE suppress.
	// Defer FPSCR across lanes so FPRF is from ps0 and stickies OR both lanes.
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	double r0 = ppc_fdiv(hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0);
	const bool s0 = ppc_fma_was_suppressed();
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	double r1 = ppc_fdiv(hCPU->fpr[frA].fp1, hCPU->fpr[frB].fp1);
	const bool s1 = ppc_fma_was_suppressed();
	ppc_ps_fma_note_suppress();
	// Always single-pack for OX/FPRF (even when VE/ZE suppress the write —
	// suite mixed SNaN+overflow still wants OX|XX from the overflow lane).
	r0 = ppc_ps_pack_arith(r0);
	r1 = ppc_ps_pack_arith(r1);
	if (s0 || s1)
	{
		hCPU->fpr[frD].fp0 = prev0;
		hCPU->fpr[frD].fp1 = prev1;
	}
	else
	{
		hCPU->fpr[frD].fp0 = r0;
		hCPU->fpr[frD].fp1 = r1;
	}
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}


// Paired-single *S style: numeric → float + FTZ; NaN/Inf keep double-form payload
// (check_ps_nan bit-compares ps0 as a double against the expected quieted NaN).
static inline double ppc_ps_round_slot(double r, bool flushDenorm)
{
	const uint64 bits = *(uint64*)&r;
	if (IS_NAN(bits) || ((bits & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL))
		return r;
	float f = (float)r;
	if (flushDenorm)
		f = flushDenormalToZero(f);
	return (double)f;
}

// PS single-domain FMA: compute both lanes, then commit with whole-register VE
// suppress (invalid on either lane leaves both prev values).
enum class PsFmaSOp { Madd, Msub, Nmadd, Nmsub };

static inline double ppc_ps_fma_s_call(PsFmaSOp op, double a, double c, double b)
{
	switch (op)
	{
	case PsFmaSOp::Madd:  return ppc_fmadds(a, c, b);
	case PsFmaSOp::Msub:  return ppc_fmsubs(a, c, b);
	case PsFmaSOp::Nmadd: return ppc_fnmadds(a, c, b);
	case PsFmaSOp::Nmsub: return ppc_fnmsubs(a, c, b);
	}
	return 0.0;
}

static inline void ppc_ps_fma_both(double& d0, double& d1,
	double a0, double c0, double b0,
	double a1, double c1, double b1,
	PsFmaSOp op)
{
	const double prev0 = d0;
	const double prev1 = d1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_ps_fma_s_call(op, a0, c0, b0);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_ps_fma_s_call(op, a1, c1, b1);
	ppc_ps_fma_note_suppress();
	d0 = ppc_ps_fma_commit_lane(prev0, r0);
	d1 = ppc_ps_fma_commit_lane(prev1, r1);
	// FPRF from would-be ps0 (even if VE left d0 as prev).
	ppc_fpscr_defer_end_single(r0);
}

void PPCInterpreter_PS_MADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Fused A·C+B; 25-bit frC inside ppc_fmadds*. No FTZ (suite denorm sticky).
	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, hCPU->fpr[frC].fp1, hCPU->fpr[frB].fp1,
		PsFmaSOp::Madd);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NMADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, hCPU->fpr[frC].fp1, hCPU->fpr[frB].fp1,
		PsFmaSOp::Nmadd);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frA, frB, frC;
	frC = (Opcode >> 6) & 0x1F;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	frD = (Opcode >> 21) & 0x1F;

	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, hCPU->fpr[frC].fp1, hCPU->fpr[frB].fp1,
		PsFmaSOp::Msub);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NMSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frA, frB, frC;
	frC = (Opcode >> 6) & 0x1F;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	frD = (Opcode >> 21) & 0x1F;

	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, hCPU->fpr[frC].fp1, hCPU->fpr[frB].fp1,
		PsFmaSOp::Nmsub);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MADDS0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Both lanes use frC.ps0 (raw; 25-bit inside helper).
	const double c = hCPU->fpr[frC].fp0;
	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, c, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, c, hCPU->fpr[frB].fp1,
		PsFmaSOp::Madd);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MADDS1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double c = hCPU->fpr[frC].fp1;
	ppc_ps_fma_both(hCPU->fpr[frD].fp0, hCPU->fpr[frD].fp1,
		hCPU->fpr[frA].fp0, c, hCPU->fpr[frB].fp0,
		hCPU->fpr[frA].fp1, c, hCPU->fpr[frB].fp1,
		PsFmaSOp::Madd);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_SEL(PPCInterpreter_t* hCPU, uint32 Opcode)
{	
	FPUCheckAvailable();

	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;


	if( hCPU->fpr[frA].fp0 >= -0.0f )
		hCPU->fpr[frD].fp0 = hCPU->fpr[frC].fp0;
	else
		hCPU->fpr[frD].fp0 = hCPU->fpr[frB].fp0;

	if( hCPU->fpr[frA].fp1 >= -0.0f )
		hCPU->fpr[frD].fp1 = hCPU->fpr[frC].fp1;
	else
		hCPU->fpr[frD].fp1 = hCPU->fpr[frB].fp1;

	// ps_sel does not touch FPSCR; Rc still copies CR1 ← FPSCR field 0.
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_SUM0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// ps0 = A.ps0 + B.ps1; ps1 = C.ps1 (copy). VE on sum → leave whole frD.
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_fma_bind_dest(prev0);
	const double sum = ppc_fadds(hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp1);
	if (ppc_fma_was_suppressed())
	{
		hCPU->fpr[frD].fp0 = prev0;
		hCPU->fpr[frD].fp1 = prev1;
	}
	else
	{
		hCPU->fpr[frD].fp0 = sum;
		hCPU->fpr[frD].fp1 = ppc_ps_quantize(hCPU->fpr[frC].fp1);
	}
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_SUM1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// ps0 = C.ps0 (copy); ps1 = A.ps0 + B.ps1. VE on sum → leave whole frD.
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_fma_bind_dest(prev1);
	const double sum = ppc_fadds(hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp1);
	if (ppc_fma_was_suppressed())
	{
		hCPU->fpr[frD].fp0 = prev0;
		hCPU->fpr[frD].fp1 = prev1;
	}
	else
	{
		hCPU->fpr[frD].fp0 = ppc_ps_quantize(hCPU->fpr[frC].fp0);
		hCPU->fpr[frD].fp1 = sum;
	}
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MULS0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double c = hCPU->fpr[frC].fp0;
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_fmuls(hCPU->fpr[frA].fp0, c);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fmuls(hCPU->fpr[frA].fp1, c);
	ppc_ps_fma_note_suppress();
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MULS1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double c = hCPU->fpr[frC].fp1;
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_fmuls(hCPU->fpr[frA].fp0, c);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fmuls(hCPU->fpr[frA].fp1, c);
	ppc_ps_fma_note_suppress();
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MR(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	// Quantize: excess-range doubles → single; SNaN not quieted.
	hCPU->fpr[frD].fp0 = ppc_ps_quantize(hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = ppc_ps_quantize(hCPU->fpr[frB].fp1);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NEG(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Bit sign flip after quantize — arithmetic `-` would quiet SNaN.
	const double q0 = ppc_ps_quantize(hCPU->fpr[frB].fp0);
	const double q1 = ppc_ps_quantize(hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0int = (*(const uint64*)&q0) ^ (1ULL << 63);
	hCPU->fpr[frD].fp1int = (*(const uint64*)&q1) ^ (1ULL << 63);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_ABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double q0 = ppc_ps_quantize(hCPU->fpr[frB].fp0);
	const double q1 = ppc_ps_quantize(hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0int = (*(const uint64*)&q0) & ~(1ULL << 63);
	hCPU->fpr[frD].fp1int = (*(const uint64*)&q1) & ~(1ULL << 63);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double q0 = ppc_ps_quantize(hCPU->fpr[frB].fp0);
	const double q1 = ppc_ps_quantize(hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0int = (*(const uint64*)&q0) | (1ULL << 63);
	hCPU->fpr[frD].fp1int = (*(const uint64*)&q1) | (1ULL << 63);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_RSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	// Whole-register VE suppress for mixed SNaN / negative lanes.
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	// Clear host FZ so denorm lane bits survive the double call ABI.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	const double b0 = hCPU->fpr[frB].fp0;
	const double b1 = hCPU->fpr[frB].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_ps_fold_estimate(b0, ppc_frsqrte(b0));
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_ps_fold_estimate(b1, ppc_frsqrte(b1));
	ppc_ps_fma_note_suppress();
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	// FPRF from would-be ps0 (suite excess-range + VE mixed).
	ppc_fpscr_defer_end_double(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE00(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;
	// Read both before write (frD may alias frA/frB).
	// Dest slot0 RN, slot1 truncate (suite excess-range merge).
	const double s0 = ppc_ps_quantize(hCPU->fpr[frA].fp0);
	const double s1 = ppc_ps_quantize_tz(hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE01(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double s0 = ppc_ps_quantize(hCPU->fpr[frA].fp0);
	const double s1 = ppc_ps_quantize_tz(hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE10(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double s0 = ppc_ps_quantize(hCPU->fpr[frA].fp1);
	const double s1 = ppc_ps_quantize_tz(hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE11(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	const double s0 = ppc_ps_quantize(hCPU->fpr[frA].fp1);
	const double s1 = ppc_ps_quantize_tz(hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_RES(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	const double prev0 = hCPU->fpr[frD].fp0;
	const double prev1 = hCPU->fpr[frD].fp1;
	ppc_ps_fma_reset_suppress();
	ppc_fpscr_defer_begin();
	ppc_fma_bind_dest(prev0);
	const double r0 = ppc_fres(hCPU->fpr[frB].fp0);
	ppc_ps_fma_note_suppress();
	ppc_fma_bind_dest(prev1);
	const double r1 = ppc_fres(hCPU->fpr[frB].fp1);
	ppc_ps_fma_note_suppress();
	hCPU->fpr[frD].fp0 = ppc_ps_fma_commit_lane(prev0, r0);
	hCPU->fpr[frD].fp1 = ppc_ps_fma_commit_lane(prev1, r1);
	ppc_fpscr_defer_end_single(r0);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

// PS compare

void PPCInterpreter_PS_CMPO0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 crfD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	crfD = (Opcode>>23)&0x7;

	ppc_fcmp_common(hCPU, crfD * 4, hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0, true);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_CMPU0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 crfD, frA, frB;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	crfD = (Opcode >> 21) & (0x7<<2);
	fcmpu_espresso(hCPU, crfD, hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0);
	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_CMPU1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 crfD, frA, frB;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	crfD = (Opcode >> 21) & (0x7 << 2);
	fcmpu_espresso(hCPU, crfD, hCPU->fpr[frA].fp1, hCPU->fpr[frB].fp1);
	PPCInterpreter_nextInstruction(hCPU);
}

// ps_cmpo1 — ordered compare of the high (ps1) slot.
void PPCInterpreter_PS_CMPO1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 crfD, frA, frB;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	crfD = (Opcode >> 23) & 0x7;

	ppc_fcmp_common(hCPU, crfD * 4, hCPU->fpr[frA].fp1, hCPU->fpr[frB].fp1, true);

	PPCInterpreter_nextInstruction(hCPU);
}
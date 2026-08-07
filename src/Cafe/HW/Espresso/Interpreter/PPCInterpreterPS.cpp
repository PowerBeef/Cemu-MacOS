#include "PPCInterpreterInternal.h"
#include <cmath>

// Gekko paired single math

void PPCInterpreter_PS_ADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0 = (float)(hCPU->fpr[frA].fp0 + hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = (float)(hCPU->fpr[frA].fp1 + hCPU->fpr[frB].fp1);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_SUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0 = (float)(hCPU->fpr[frA].fp0 - hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = (float)(hCPU->fpr[frA].fp1 - hCPU->fpr[frB].fp1);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MUL(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0 = flushDenormalToZero((float)(hCPU->fpr[frA].fp0 * roundTo25BitAccuracy(hCPU->fpr[frC].fp0)));
	hCPU->fpr[frD].fp1 = flushDenormalToZero((float)(hCPU->fpr[frA].fp1 * roundTo25BitAccuracy(hCPU->fpr[frC].fp1)));

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_DIV(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0 = (float)(hCPU->fpr[frA].fp0 / hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = (float)(hCPU->fpr[frA].fp1 / hCPU->fpr[frB].fp1);

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

void PPCInterpreter_PS_MADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB, frC;
	frC = (Opcode>>6)&0x1F;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	// Fused A·C25+B; ppc750cl.s: 'fmadds/ps_madd is not rounded twice'.
	const double r0 = ppc_fmadd(hCPU->fpr[frA].fp0, roundTo25BitAccuracy(hCPU->fpr[frC].fp0), hCPU->fpr[frB].fp0);
	const double r1 = ppc_fmadd(hCPU->fpr[frA].fp1, roundTo25BitAccuracy(hCPU->fpr[frC].fp1), hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, true);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, true);

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

	const double r0 = ppc_fnmadd(hCPU->fpr[frA].fp0, roundTo25BitAccuracy(hCPU->fpr[frC].fp0), hCPU->fpr[frB].fp0);
	const double r1 = ppc_fnmadd(hCPU->fpr[frA].fp1, roundTo25BitAccuracy(hCPU->fpr[frC].fp1), hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, false);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, false);

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

	const double r0 = ppc_fmsub(hCPU->fpr[frA].fp0, roundTo25BitAccuracy(hCPU->fpr[frC].fp0), hCPU->fpr[frB].fp0);
	const double r1 = ppc_fmsub(hCPU->fpr[frA].fp1, roundTo25BitAccuracy(hCPU->fpr[frC].fp1), hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, false);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, false);

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

	const double r0 = ppc_fnmsub(hCPU->fpr[frA].fp0, roundTo25BitAccuracy(hCPU->fpr[frC].fp0), hCPU->fpr[frB].fp0);
	const double r1 = ppc_fnmsub(hCPU->fpr[frA].fp1, roundTo25BitAccuracy(hCPU->fpr[frC].fp1), hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, false);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, false);

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

	const double c = roundTo25BitAccuracy(hCPU->fpr[frC].fp0);
	const double r0 = ppc_fmadd(hCPU->fpr[frA].fp0, c, hCPU->fpr[frB].fp0);
	const double r1 = ppc_fmadd(hCPU->fpr[frA].fp1, c, hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, false);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, false);

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

	const double c = roundTo25BitAccuracy(hCPU->fpr[frC].fp1);
	const double r0 = ppc_fmadd(hCPU->fpr[frA].fp0, c, hCPU->fpr[frB].fp0);
	const double r1 = ppc_fmadd(hCPU->fpr[frA].fp1, c, hCPU->fpr[frB].fp1);
	hCPU->fpr[frD].fp0 = ppc_ps_round_slot(r0, false);
	hCPU->fpr[frD].fp1 = ppc_ps_round_slot(r1, false);

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

	float s0 = (float)(hCPU->fpr[frA].fp0 + hCPU->fpr[frB].fp1);
	float s1 = (float)hCPU->fpr[frC].fp1;

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

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

	float s0 = (float)hCPU->fpr[frC].fp0;
	float s1 = (float)(hCPU->fpr[frA].fp0 + hCPU->fpr[frB].fp1);

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MULS0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	double c = roundTo25BitAccuracy(hCPU->fpr[frC].fp0);
	float s0 = (float)(hCPU->fpr[frA].fp0 * c);
	float s1 = (float)(hCPU->fpr[frA].fp1 * c);

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MULS1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frC;
	frC = (Opcode>>6)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	double c = roundTo25BitAccuracy(hCPU->fpr[frC].fp1);
	float s0 = (float)(hCPU->fpr[frA].fp0 * c);
	float s1 = (float)(hCPU->fpr[frA].fp1 * c);

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MR(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	hCPU->fpr[frD].fp0 = hCPU->fpr[frB].fp0;
	hCPU->fpr[frD].fp1 = hCPU->fpr[frB].fp1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NEG(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0 = -hCPU->fpr[frB].fp0;
	hCPU->fpr[frD].fp1 = -hCPU->fpr[frB].fp1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_ABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0int = hCPU->fpr[frB].fp0int & ~(1ULL << 63);
	hCPU->fpr[frD].fp1int = hCPU->fpr[frB].fp1int & ~(1ULL << 63);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_NABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;

	hCPU->fpr[frD].fp0int = hCPU->fpr[frB].fp0int | (1ULL << 63);
	hCPU->fpr[frD].fp1int = hCPU->fpr[frB].fp1int | (1ULL << 63);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_RSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	hCPU->fpr[frD].fp0 = (float)frsqrte_espresso(hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = (float)frsqrte_espresso(hCPU->fpr[frB].fp1);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE00(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;
	double s0 = hCPU->fpr[frA].fp0;
	double s1 = hCPU->fpr[frB].fp0;
	
	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE01(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	double s0 = hCPU->fpr[frA].fp0;
	double s1 = hCPU->fpr[frB].fp1;

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE10(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	double s0 = hCPU->fpr[frA].fp1;
	double s1 = hCPU->fpr[frB].fp0;

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_MERGE11(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frA, frB;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	frD = (Opcode>>21)&0x1F;

	double s0 = hCPU->fpr[frA].fp1;
	double s1 = hCPU->fpr[frB].fp1;

	hCPU->fpr[frD].fp0 = s0;
	hCPU->fpr[frD].fp1 = s1;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_PS_RES(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	sint32 frD, frB;
	frB = (Opcode>>11)&0x1F;
	frD = (Opcode>>21)&0x1F;
	
	hCPU->fpr[frD].fp0 = (float)fres_espresso(hCPU->fpr[frB].fp0);
	hCPU->fpr[frD].fp1 = (float)fres_espresso(hCPU->fpr[frB].fp1);

	PPCInterpreter_nextInstruction(hCPU);
}

// PS compare

void PPCInterpreter_PS_CMPO0(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 crfD, frA, frB;
	uint32 c=0;
	frB = (Opcode>>11)&0x1F;
	frA = (Opcode>>16)&0x1F;
	crfD = (Opcode>>23)&0x7;


	double a = hCPU->fpr[frA].fp0;
	double b = hCPU->fpr[frB].fp0;

	ppc_setCRBit(hCPU, crfD*4+0, 0);
	ppc_setCRBit(hCPU, crfD*4+1, 0);
	ppc_setCRBit(hCPU, crfD*4+2, 0);
	ppc_setCRBit(hCPU, crfD*4+3, 0);

	if(IS_NAN(*(uint64*)&a) || IS_NAN(*(uint64*)&b))
	{
		c = 1;
		ppc_setCRBit(hCPU, crfD*4+CR_BIT_SO, 1);
	}
	else if(a < b)
	{
		c = 8;
		ppc_setCRBit(hCPU, crfD*4+CR_BIT_LT, 1);
	}
	else if(a > b)
	{
		c = 4;
		ppc_setCRBit(hCPU, crfD*4+CR_BIT_GT, 1);
	}
	else
	{
		c = 2;
		ppc_setCRBit(hCPU, crfD*4+CR_BIT_EQ, 1);
	}

	hCPU->fpscr = (hCPU->fpscr & 0xffff0fff) | (c << 12);
	
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

// ps_cmpo1 — ordered compare of the high (ps1) slot. Was missing entirely: the primary-4
// compare sub-dispatch only had CMPU0/CMPO0/CMPU1, so case 3 fell into
// cemu_assert_unimplemented. 14 values-only suite failures.
void PPCInterpreter_PS_CMPO1(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	sint32 crfD, frA, frB;
	uint32 c = 0;
	frB = (Opcode >> 11) & 0x1F;
	frA = (Opcode >> 16) & 0x1F;
	crfD = (Opcode >> 23) & 0x7;

	double a = hCPU->fpr[frA].fp1;
	double b = hCPU->fpr[frB].fp1;

	ppc_setCRBit(hCPU, crfD * 4 + 0, 0);
	ppc_setCRBit(hCPU, crfD * 4 + 1, 0);
	ppc_setCRBit(hCPU, crfD * 4 + 2, 0);
	ppc_setCRBit(hCPU, crfD * 4 + 3, 0);

	if (IS_NAN(*(uint64*)&a) || IS_NAN(*(uint64*)&b))
	{
		c = 1;
		ppc_setCRBit(hCPU, crfD * 4 + CR_BIT_SO, 1);
	}
	else if (a < b)
	{
		c = 8;
		ppc_setCRBit(hCPU, crfD * 4 + CR_BIT_LT, 1);
	}
	else if (a > b)
	{
		c = 4;
		ppc_setCRBit(hCPU, crfD * 4 + CR_BIT_GT, 1);
	}
	else
	{
		c = 2;
		ppc_setCRBit(hCPU, crfD * 4 + CR_BIT_EQ, 1);
	}

	hCPU->fpscr = (hCPU->fpscr & 0xffff0fff) | (c << 12);
	// Match FCMPO / ordered semantics for SNaN and VXVC (PS_CMPO0 is missing these flags
	// and is left alone for now so this change is bisectable).
	if (IS_SNAN(*(uint64*)&a) || IS_SNAN(*(uint64*)&b))
		hCPU->fpscr |= FPSCR_VXSNAN;
	else if (!(hCPU->fpscr & FPSCR_VE) || IS_QNAN(*(uint64*)&a) || IS_QNAN(*(uint64*)&b))
		hCPU->fpscr |= FPSCR_VXVC;

	PPCInterpreter_nextInstruction(hCPU);
}
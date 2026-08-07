#pragma once

#include "Cafe/HW/Espresso/PPCState.h"
#include <cstring>

// SPR constants
#define SPR_XER		1	
#define SPR_LR		8	
#define SPR_CTR		9	
#define SPR_DEC		22	
#define SPR_SRR0	26	
#define SPR_SRR1	27	
#define SPR_HID0	1008
#define SPR_HID1	1009
#define SPR_HID2	920	
#define SPR_TBL		268	
#define SPR_TBU		269	
#define SPR_DMAU	922	
#define SPR_DMAL	923	

// graphics quantization registers
#define SPR_GQR0 912
#define SPR_GQR1 913
#define SPR_GQR2 914
#define SPR_GQR3 915
#define SPR_GQR4 916
#define SPR_GQR5 917
#define SPR_GQR6 918
#define SPR_GQR7 919

// user graphics quantization registers
#define SPR_UGQR0	896
#define SPR_UGQR1	897
#define SPR_UGQR2	898
#define SPR_UGQR3	899
#define SPR_UGQR4	900
#define SPR_UGQR5	901
#define SPR_UGQR6	902
#define SPR_UGQR7	903

#define SPR_FPECR	1022	// used by the OS to store values

#define SPR_PVR		287		// processor version, for Wii U this must be 0x7001xxxx - this register is only readable
#define SPR_UPIR	1007	// core index
#define SPR_SCR		947		// core control
#define SPR_SDR1	25

// reversed CR bit indices
#define CR_BIT_LT	0
#define CR_BIT_GT	1
#define CR_BIT_EQ	2
#define CR_BIT_SO	3

#define XER_BIT_CA		(29)	// carry bit index. To accelerate frequent access, this bit is stored as a separate uint8
#define XER_BIT_SO		(31)	// summary overflow, counterpart to CR SO
#define XER_BIT_OV		(30)

// FPSCR (host bit = 1 << (31 - ppc_bit), matching PEM / Dolphin)
#define FPSCR_FX		(1u << 31)
#define FPSCR_FEX		(1u << 30)
#define FPSCR_VX		(1u << 29)
#define FPSCR_OX		(1u << 28)
#define FPSCR_UX		(1u << 27)
#define FPSCR_ZX		(1u << 26)
#define FPSCR_XX		(1u << 25)
#define FPSCR_VXSNAN	(1u << 24)
#define FPSCR_VXISI		(1u << 23)
#define FPSCR_VXIDI		(1u << 22)
#define FPSCR_VXZDZ		(1u << 21)
#define FPSCR_VXIMZ		(1u << 20)
#define FPSCR_VXVC		(1u << 19)
#define FPSCR_VXSOFT	(1u << 10)
#define FPSCR_VXSQRT	(1u << 9)
#define FPSCR_VXCVI		(1u << 8)
#define FPSCR_VE		(1u << 7)	// also used by FMA VE suppress
#define FPSCR_OE		(1u << 6)
#define FPSCR_UE		(1u << 5)
#define FPSCR_ZE		(1u << 4)	// also used by FMA ZE suppress
#define FPSCR_XE		(1u << 3)
// FPRF field (PPC bits 15–19) lives at host bits 12–16.
// Encodings match ppc750cl check_fpu_* (ori/oris into expected FPSCR).
#define FPSCR_FPRF_MASK	(0x1Fu << 12)
#define FPSCR_FPRF_PN	(0x4u << 12)	// +normal   (suite 0x4000)
#define FPSCR_FPRF_NN	(0x8u << 12)	// −normal   (suite 0x8000)
#define FPSCR_FPRF_PZ	(0x2u << 12)	// +zero     (suite 0x2000)
#define FPSCR_FPRF_NZ	(0x12u << 12)	// −zero     (suite 0x12000)
#define FPSCR_FPRF_PD	(0x14u << 12)	// +denorm   (suite 0x14000)
#define FPSCR_FPRF_ND	(0x18u << 12)	// −denorm   (suite 0x18000)
#define FPSCR_FPRF_PINF	(0x5u << 12)	// +inf      (suite 0x5000)
#define FPSCR_FPRF_NINF	(0x9u << 12)	// −inf      (suite 0x9000)
#define FPSCR_FPRF_QNAN	(0x11u << 12)	// QNaN      (suite 0x11000)
// FI/FR: PPC bits 14/13 → host 17/18. Suite tests FI; FR is ignored by check_fpscr.
#define FPSCR_FI		(1u << 17)
#define FPSCR_FR		(1u << 18)
#define FPSCR_VX_ANY	(FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | \
			 FPSCR_VXVC | FPSCR_VXSOFT | FPSCR_VXSQRT | FPSCR_VXCVI)
#define FPSCR_ANY_X	(FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ANY)
#define FPSCR_ANY_E	(FPSCR_VE | FPSCR_OE | FPSCR_UE | FPSCR_ZE | FPSCR_XE)
// Bit 20 (PPC) / host bit 11 is reserved; hardware ignores writes to it.
#define FPSCR_RESERVED_MASK	(0xFFFFF7FFu)

// Full FPRF class set (including denorms). Double-precision result domain.
static inline void ppc_fpscr_set_fprf_from_double(uint32& fpscr, double v)
{
	uint64 bits;
	std::memcpy(&bits, &v, sizeof(bits));
	const uint64 abs = bits & 0x7FFFFFFFFFFFFFFFULL;
	const bool sign = (bits >> 63) != 0;
	uint32 cls;
	if (abs > 0x7FF0000000000000ULL)
		cls = FPSCR_FPRF_QNAN;
	else if (abs == 0x7FF0000000000000ULL)
		cls = sign ? FPSCR_FPRF_NINF : FPSCR_FPRF_PINF;
	else if (abs == 0)
		cls = sign ? FPSCR_FPRF_NZ : FPSCR_FPRF_PZ;
	else if (abs < 0x0010000000000000ULL)
		cls = sign ? FPSCR_FPRF_ND : FPSCR_FPRF_PD; // subnormal
	else
		cls = sign ? FPSCR_FPRF_NN : FPSCR_FPRF_PN;
	fpscr = (fpscr & ~FPSCR_FPRF_MASK) | cls;
}

// FPRF in the *single-precision* domain (frsp / single-arith results).
// A min single denorm re-expanded to double looks like a normal double, but
// suite check_fpu_pdenorm expects the single-domain class (0x14000 / 0x18000).
static inline void ppc_fpscr_set_fprf_from_single(uint32& fpscr, float v)
{
	uint32 bits;
	std::memcpy(&bits, &v, sizeof(bits));
	const uint32 abs = bits & 0x7FFFFFFFu;
	const bool sign = (bits >> 31) != 0;
	uint32 cls;
	if (abs > 0x7F800000u)
		cls = FPSCR_FPRF_QNAN;
	else if (abs == 0x7F800000u)
		cls = sign ? FPSCR_FPRF_NINF : FPSCR_FPRF_PINF;
	else if (abs == 0)
		cls = sign ? FPSCR_FPRF_NZ : FPSCR_FPRF_PZ;
	else if (abs < 0x00800000u)
		cls = sign ? FPSCR_FPRF_ND : FPSCR_FPRF_PD;
	else
		cls = sign ? FPSCR_FPRF_NN : FPSCR_FPRF_PN;
	fpscr = (fpscr & ~FPSCR_FPRF_MASK) | cls;
}

// After any FPSCR write: drop reserved bit, recompute VX/FEX (Dolphin-compatible).
static inline void ppc_fpscr_recompute(uint32& fpscr)
{
	fpscr &= FPSCR_RESERVED_MASK;
	if (fpscr & FPSCR_VX_ANY)
		fpscr |= FPSCR_VX;
	else
		fpscr &= ~FPSCR_VX;
	// Exception stickies shifted by 22 line up with enable bits; VX lines up with VE.
	if (((fpscr >> 22) & (fpscr & FPSCR_ANY_E)) != 0)
		fpscr |= FPSCR_FEX;
	else
		fpscr &= ~FPSCR_FEX;
}

// OR exception stickies; set FX when a sticky in ANY_X transitions 0→1.
static inline void ppc_fpscr_or_sticky(uint32& fpscr, uint32 sticky_or)
{
	const uint32 newX = sticky_or & FPSCR_ANY_X;
	if (newX != 0 && (fpscr & newX) != newX)
		fpscr |= FPSCR_FX;
	fpscr |= sticky_or;
}

// single_domain: FPRF from float view (frsp / *s). Else from double bits.
// update_fprf=false for check_fpu_noresult_nofprf (VE fctiw abort).
static inline void ppc_fpscr_commit_result(uint32& fpscr, double result, uint32 sticky_or,
	bool set_fi, bool update_fprf = true, bool single_domain = false)
{
	fpscr &= ~(FPSCR_FI | FPSCR_FR);
	if (update_fprf)
	{
		if (single_domain)
			ppc_fpscr_set_fprf_from_single(fpscr, (float)result);
		else
			ppc_fpscr_set_fprf_from_double(fpscr, result);
	}
	if (set_fi)
		fpscr |= FPSCR_FI;
	if (sticky_or != 0)
		ppc_fpscr_or_sticky(fpscr, sticky_or);
	// XX without FI is wrong for ordinary arith; callers pass XX|FI together.
	// fres/ps_res are the FI-without-XX special case (suite add_fpscr_fi).
	ppc_fpscr_recompute(fpscr);
}

// CR1 ← FPSCR field 0 (FX, FEX, VX, OX) for Rc forms of FP ops.
static inline void ppc_fpscr_update_cr1(PPCInterpreter_t* hCPU)
{
	const uint32 f = hCPU->fpscr;
	hCPU->cr[4] = (f & FPSCR_FX) ? 1 : 0;
	hCPU->cr[5] = (f & FPSCR_FEX) ? 1 : 0;
	hCPU->cr[6] = (f & FPSCR_VX) ? 1 : 0;
	hCPU->cr[7] = (f & FPSCR_OX) ? 1 : 0;
}

#define MSR_SF			(1<<31)
#define MSR_UNKNOWN		(1<<30)
#define MSR_UNKNOWN2	(1<<27)
#define MSR_VEC			(1<<25)
#define MSR_POW			(1<<18)
#define MSR_TGPR		(1<<15)
#define MSR_ILE			(1<<16)
#define MSR_EE			(1<<15)
#define MSR_PR			(1<<14)
#define MSR_FP			(1<<13)
#define MSR_ME			(1<<12)
#define MSR_FE0			(1<<11)
#define MSR_SE			(1<<10)
#define MSR_BE			(1<<9)
#define MSR_FE1			(1<<8)
#define MSR_IP			(1<<6)
#define MSR_IR			(1<<5)
#define MSR_DR			(1<<4)
#define MSR_PM			(1<<2)
#define MSR_RI			(1<<1)
#define MSR_LE			(1)

// helpers

#define GET_MSR_BIT(__bit) ((hCPU->sprExtended.msr&(__bit))!=0)

#define opHasRC() ((opcode & PPC_OPC_RC) != 0)

// assume fixed values for PSE/LSQE. This optimization is possible because Wii U applications run only in user mode (todo - handle this correctly in LLE mode)
//#define PPC_LSQE	(hCPU->LSQE)
//#define PPC_PSE	(hCPU->PSE)

#define PPC_LSQE		(1)
#define PPC_PSE			(1)

#define PPC_ASSERT(v)

#define PPC_OPC_RC		1
#define PPC_OPC_OE		(1<<10)
#define PPC_OPC_LK		1
#define PPC_OPC_AA		(1<<1)

#define PPC_OPC_TEMPL_A(opc, rD, rA, rB, rC) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;rC=((opc)>>6)&0x1f;}
#define PPC_OPC_TEMPL_B(opc, BO, BI, BD) {BO=((opc)>>21)&0x1f;BI=((opc)>>16)&0x1f;BD=(uint32)(sint32)(sint16)((opc)&0xfffc);}
#define PPC_OPC_TEMPL_D_SImm(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(uint32)(sint32)(sint16)((opc)&0xffff);}
#define PPC_OPC_TEMPL_D_UImm(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(opc)&0xffff;}
#define PPC_OPC_TEMPL_D_Shift16(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(opc)<<16;}
#define PPC_OPC_TEMPL_I(opc, LI) {LI=(opc)&0x3fffffc;if (LI&0x02000000) LI |= 0xfc000000;}
#define PPC_OPC_TEMPL_M(opc, rS, rA, SH, MB, ME) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;SH=((opc)>>11)&0x1f;MB=((opc)>>6)&0x1f;ME=((opc)>>1)&0x1f;}
#define PPC_OPC_TEMPL_X(opc, rS, rA, rB) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XFX(opc, rS, CRM) {rS=((opc)>>21)&0x1f;CRM=((opc)>>12)&0xff;}
#define PPC_OPC_TEMPL_XO(opc, rS, rA, rB) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XL(opc, BO, BI, BD) {BO=((opc)>>21)&0x1f;BI=((opc)>>16)&0x1f;BD=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XFL(opc, rB, FM) {rB=((opc)>>11)&0x1f;FM=((opc)>>17)&0xff;}

#define PPC_OPC_TEMPL3_XO() sint32 rD, rA, rB; rD=((opcode)>>21)&0x1f;rA=((opcode)>>16)&0x1f;rB=((opcode)>>11)&0x1f
#define PPC_OPC_TEMPL_X_CR() sint32 crD, crA, crB; crD=((opcode)>>21)&0x1f;crA=((opcode)>>16)&0x1f;crB=((opcode)>>11)&0x1f

static inline void ppc_update_cr0(PPCInterpreter_t* hCPU, uint32 r)
{
	cemu_assert_debug(hCPU->xer_so <= 1);
	hCPU->cr[CR_BIT_SO] = hCPU->xer_so;
	hCPU->cr[CR_BIT_LT] = ((r != 0) ? 1 : 0) & ((r & 0x80000000) ? 1 : 0);
	hCPU->cr[CR_BIT_EQ] = (r == 0);
	hCPU->cr[CR_BIT_GT] = hCPU->cr[CR_BIT_EQ] ^ hCPU->cr[CR_BIT_LT] ^ 1;  // this works because EQ and LT can never be set at the same time. So the only case where GT becomes 1 is when LT=0 and EQ=0
}

static inline uint8 ppc_getCRBit(PPCInterpreter_t* hCPU, uint32 r)
{
	return hCPU->cr[r];
}

static inline bool ppc_MTCRFMaskHasCRFieldSet(const uint32 mtcrfMask, const uint32 crIndex)
{
	// 1000 0000 (0x80) -> cr0
	// 0000 0001 (0x01) -> cr7
	return (mtcrfMask & (1 << (7 - crIndex))) != 0;
}

// returns CR mask with CR0.LT in LSB
static inline uint32 ppc_MTCRFMaskToCRBitMask(const uint32 mtcrfMask)
{
	uint32 crMask = 0; 
	for (uint32 crF = 0; crF < 8; crF++)
	{
		if (ppc_MTCRFMaskHasCRFieldSet(mtcrfMask, crF))
			crMask |= (0xF << (crF * 4));
	}
	return crMask;
}

static inline void ppc_setCRBit(PPCInterpreter_t* hCPU, uint32 r, uint8 v)
{
	hCPU->cr[r] = v;
}

static inline void ppc_setCR(PPCInterpreter_t* hCPU, uint32 cr)
{
	uint32 tempCr = cr;
	for (sint32 i = 31; i >= 0; i--)
	{
		ppc_setCRBit(hCPU, i, tempCr & 1);
		tempCr >>= 1;
	}
}

static inline uint32 ppc_getCR(PPCInterpreter_t* hCPU)
{
	uint32 cr = 0;
	for (sint32 i = 0; i < 32; i++)
	{
		cr <<= 1;
		if (ppc_getCRBit(hCPU, i))
			cr |= 1;
	}
	return cr;
}

// FPU helper

#define IS_NAN(X)				((((X) & 0x000fffffffffffffULL) != 0) && (((X) & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL))
#define IS_QNAN(X)				((((X) & 0x000fffffffffffffULL) != 0) && (((X) & 0x7ff8000000000000ULL) == 0x7ff8000000000000ULL))
#define IS_SNAN(X)				((((X) & 0x000fffffffffffffULL) != 0) && (((X) & 0x7ff8000000000000ULL) == 0x7ff0000000000000ULL))

// Espresso single-precision multiply factor: 25-bit mantissa for frC.
// Non-inline so the recompiler can call it via make_call_imm (same ABI as fres_espresso).
ATTR_MS_ABI double roundTo25BitAccuracy(double d);

// PowerPC fused multiply-add family with Espresso NaN selection
// (order frA → frB → frC; SNaN quieted; 0·∞ → default QNaN; nmadd does not
// flip NaN sign). Args are (frA, frC, frB) matching the product A·C ± B.
// Callables from the recompiler via make_call_imm.
//
// Call ppc_fma_bind_dest(current frD) immediately before each helper so that
// FPSCR[VE] can suppress the write (return the prior value) on invalid ops.
ATTR_MS_ABI void ppc_fma_bind_dest(double prevFrD);
// True after a helper returned prevFrD because VE blocked an invalid op.
ATTR_MS_ABI bool ppc_fma_was_suppressed();
// PS two-lane ops: accumulate FPSCR stickies/FI, commit once with FPRF from ps0.
ATTR_MS_ABI void ppc_fpscr_defer_begin();
ATTR_MS_ABI void ppc_fpscr_defer_end_single(double ps0_result);
ATTR_MS_ABI void ppc_fpscr_defer_end_double(double ps0_result);
// PS FMA: VE+invalid on either lane suppresses the whole frD write.
ATTR_MS_ABI void ppc_ps_fma_reset_suppress();
ATTR_MS_ABI void ppc_ps_fma_note_suppress();
ATTR_MS_ABI double ppc_ps_fma_commit_lane(double prev, double computed);
ATTR_MS_ABI double ppc_fmadd(double a, double c, double b);
ATTR_MS_ABI double ppc_fmsub(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmadd(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmsub(double a, double c, double b);
// Single-precision FMA domain (*S / ps_*): fmaf when operands convert cleanly
// to f32; double fma+round when frC/etc. carry excess range (HUGE_VAL).
ATTR_MS_ABI double ppc_fmadds(double a, double c, double b);
ATTR_MS_ABI double ppc_fmsubs(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmadds(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmsubs(double a, double c, double b);
// Single-precision multiply (*S / ps_mul*): raw frC, 25-bit + ldexp product.
ATTR_MS_ABI double ppc_fmuls(double a, double c);
// Double-precision mul/div with Espresso NaN order and VE/ZE suppress.
ATTR_MS_ABI double ppc_fmul(double a, double c);
ATTR_MS_ABI double ppc_fdiv(double a, double b);
ATTR_MS_ABI double ppc_fadd(double a, double b);
ATTR_MS_ABI double ppc_fsubs(double a, double b);
ATTR_MS_ABI double ppc_fadds(double a, double b);
// Compare: CR + FPSCR[FPCC]/stickies]. crBitBase = field*4. ordered → fcmpo/ps_cmpo*.
void ppc_fcmp_common(PPCInterpreter_t* hCPU, int crBitBase, double a, double b, bool ordered);
ATTR_MS_ABI void ppc_fcmpu_fpscr(double a, double b);
ATTR_MS_ABI void ppc_fcmpo_fpscr(double a, double b);
ATTR_MS_ABI double ppc_fsub(double a, double b);
// Estimates with SNaN quiet + VE/ZE suppress (bind_dest before call).
ATTR_MS_ABI double ppc_fres(double b);
ATTR_MS_ABI double ppc_frsqrte(double b);
// PS move/merge quantize: round finite to single (FZ-safe); Inf/NaN via bit
// convert so SNaNs are not quieted (suite: moves do not raise on NaN).
ATTR_MS_ABI double ppc_ps_quantize(double d);
// Merge destination slot1: freescale truncate (no RN).
ATTR_MS_ABI double ppc_ps_quantize_tz(double d);
// Fold PS estimate result when input was single-format (low 29 bits clear).
ATTR_MS_ABI double ppc_ps_fold_estimate(double input, double result);
// lfd → ps1 shadow from high word of the loaded double.
ATTR_MS_ABI double ppc_lfd_ps_shadow(double d);
ATTR_MS_ABI void ppc_isync_clear_ps_dirty();
ATTR_MS_ABI void ppc_note_ps_write(sint32 frD);
// Per-frD helpers (recompiler picks by frD at gen time).
extern uintptr_t g_lfd_ps1_fr_fn[32];
extern uintptr_t g_note_ps_write_fr_fn[32];
// After arithmetic: keep Inf/NaN double-form (check_ps_nan); quantize finites.
ATTR_MS_ABI double ppc_ps_pack_arith(double r);

ATTR_MS_ABI double fres_espresso(double input);
ATTR_MS_ABI double frsqrte_espresso(double input);

void fcmpu_espresso(PPCInterpreter_t* hCPU, int crfD, double a, double b);

// OPC
void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode);

void PPCInterpreter_MFMSR(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MTMSR(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MFTB(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MTFSB1X(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MTFSB0X(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MCRFS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MFCR(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MCRF(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MTCRF(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MCRXR(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_TLBIE(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_TLBSYNC(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_DCBT(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_DCBST(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_DCBZL(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_DCBF(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_DCBI(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_DCBZ(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_ICBI(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_EIEIO(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_SC(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_SYNC(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_ISYNC(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_RFI(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_BX(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_BCX(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_BCLRX(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_BCCTR(PPCInterpreter_t* hCPU, uint32 Opcode);

// FPU

void PPCInterpreter_FCMPO(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FCMPU(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_FMR(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FSEL(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FCTIWZ(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FCTIW(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNEG(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FRSP(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FRSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FRES(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_FABS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNABS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMUL(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FDIV(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FSUB(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMSUB(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMSUBS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNMADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNMSUB(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_MFFS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_MTFSF(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_FDIVS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMULS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FADDS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FSUBS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FMADDS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNMADDS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_FNMSUBS(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_PS_MERGE00(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MERGE01(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MERGE10(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MERGE11(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MR(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_NEG(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_ABS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_NABS(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_RES(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_RSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_PS_ADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_SUB(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MUL(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_DIV(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_PS_MADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_NMADD(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MADDS0(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MADDS1(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MSUB(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_NMSUB(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_PS_SEL(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_SUM0(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_SUM1(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MULS0(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_MULS1(PPCInterpreter_t* hCPU, uint32 Opcode);

void PPCInterpreter_PS_CMPO0(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_CMPO1(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_CMPU0(PPCInterpreter_t* hCPU, uint32 Opcode);
void PPCInterpreter_PS_CMPU1(PPCInterpreter_t* hCPU, uint32 Opcode);

// PowerPC FPSCR[RN] -- the low two bits -- selects the rounding mode, and nothing in this emulator
// read it: every conversion was a plain C++ cast or a host FP instruction, both of which use the
// HOST rounding mode, so the guest always got round-to-nearest-even no matter what it asked for.
// ppc750cl.s catches this as 68 `frsp` failures returning values like 4194304.5, which is not even
// representable in single precision and therefore cannot be a legal frsp result.
//
// Applied by setting the host mode, which covers the recompiler as well as the interpreter: both
// run guest FP on the host FPU, which is why both arms fail identically. fesetround is not free, so
// it is applied only when the guest's mode actually changes -- games change it rarely, the
// conformance suite changes it constantly, and that asymmetry is the whole design.
void PPCInterpreter_setRoundingModeFromFPSCR(PPCInterpreter_t* hCPU);
void PPCInterpreter_MTFSFI(PPCInterpreter_t* hCPU, uint32 Opcode);

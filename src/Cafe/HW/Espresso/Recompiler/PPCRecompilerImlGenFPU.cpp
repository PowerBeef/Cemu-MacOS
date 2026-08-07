#include "Cafe/HW/Espresso/EspressoISA.h"
#include "../Interpreter/PPCInterpreterInternal.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "IML/IML.h"

ATTR_MS_ABI double frsqrte_espresso(double input);
ATTR_MS_ABI double fres_espresso(double input);
ATTR_MS_ABI double roundTo25BitAccuracy(double d);
ATTR_MS_ABI void ppc_fma_bind_dest(double prevFrD);
ATTR_MS_ABI void ppc_ps_fma_reset_suppress();
ATTR_MS_ABI void ppc_ps_fma_note_suppress();
ATTR_MS_ABI double ppc_ps_fma_commit_lane(double prev, double computed);
ATTR_MS_ABI void ppc_fpscr_defer_begin();
ATTR_MS_ABI void ppc_fpscr_defer_end_single(double ps0_result);
ATTR_MS_ABI void ppc_fpscr_defer_end_double(double ps0_result);
ATTR_MS_ABI void ppc_fpscr_update_cr1_abi();
ATTR_MS_ABI double ppc_fmadd(double a, double c, double b);
ATTR_MS_ABI double ppc_fmsub(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmadd(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmsub(double a, double c, double b);
ATTR_MS_ABI double ppc_fmadds(double a, double c, double b);
ATTR_MS_ABI double ppc_fmsubs(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmadds(double a, double c, double b);
ATTR_MS_ABI double ppc_fnmsubs(double a, double c, double b);
ATTR_MS_ABI double ppc_fmuls(double a, double c);
ATTR_MS_ABI double ppc_fmul(double a, double c);
ATTR_MS_ABI double ppc_fdiv(double a, double b);
ATTR_MS_ABI double ppc_fadd(double a, double b);
ATTR_MS_ABI double ppc_fsub(double a, double b);
ATTR_MS_ABI double ppc_fres(double b);
ATTR_MS_ABI double ppc_frsqrte(double b);
ATTR_MS_ABI double ppc_frsp(double b);
ATTR_MS_ABI double ppc_fctiw(double b);
ATTR_MS_ABI double ppc_fctiwz(double b);
ATTR_MS_ABI double ppc_ps_quantize(double d);
ATTR_MS_ABI double ppc_ps_quantize_tz(double d);
ATTR_MS_ABI double ppc_ps_fold_estimate(double input, double result);
extern uintptr_t g_lfd_ps1_fr_fn[32];
extern uintptr_t g_note_ps_write_fr_fn[32];
ATTR_MS_ABI void ppc_note_ps_write(sint32 frD);
ATTR_MS_ABI void ppc_isync_clear_ps_dirty();
ATTR_MS_ABI double ppc_ps_pack_arith(double r);

// Bind current frD (for FPSCR[VE] suppress) then call a 3-arg FMA helper.
static void emit_ppc_fma_call(ppcImlGenContext_t* ctx, uintptr_t fn, IMLReg fprD, IMLReg fprA, IMLReg fprC, IMLReg fprB)
{
	ctx->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ctx->emitInst().make_call_imm(fn, fprA, fprC, fprB, fprD);
}

IMLReg _GetRegCR(ppcImlGenContext_t* ppcImlGenContext, uint8 crReg, uint8 crBit);

#define DefinePS0(name, regIndex) IMLReg name = _GetFPRRegPS0(ppcImlGenContext, regIndex);
#define DefinePS1(name, regIndex) IMLReg name = _GetFPRRegPS1(ppcImlGenContext, regIndex);
#define DefinePSX(name, regIndex, isPS1) IMLReg name = isPS1 ? _GetFPRRegPS1(ppcImlGenContext, regIndex) : _GetFPRRegPS0(ppcImlGenContext, regIndex);
#define DefineTempFPR(name, index) IMLReg name = _GetFPRTemp(ppcImlGenContext, index);

// Espresso single-precision mul/FMA: product uses frC at 25-bit mantissa accuracy.
// Writes rounded value to dest (may equal src after the call only if RA allows; prefer a temp).
static void emit_roundFrC_to25Bit(ppcImlGenContext_t* ppcImlGenContext, IMLReg dest, IMLReg src)
{
	if (dest != src)
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, dest, src);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)roundTo25BitAccuracy, dest, IMLREG_INVALID, IMLREG_INVALID, dest);
}

IMLReg _GetFPRRegPS0(ppcImlGenContext_t* ppcImlGenContext, uint32 regIndex)
{
	cemu_assert_debug(regIndex < 32);
	return PPCRecompilerImlGen_LookupReg(ppcImlGenContext, PPCREC_NAME_FPR_HALF + regIndex * 2 + 0, IMLRegFormat::F64);
}

IMLReg _GetFPRRegPS1(ppcImlGenContext_t* ppcImlGenContext, uint32 regIndex)
{
	cemu_assert_debug(regIndex < 32);
	return PPCRecompilerImlGen_LookupReg(ppcImlGenContext, PPCREC_NAME_FPR_HALF + regIndex * 2 + 1, IMLRegFormat::F64);
}

IMLReg _GetFPRTemp(ppcImlGenContext_t* ppcImlGenContext, uint32 index)
{
	cemu_assert_debug(index < 4);
	return PPCRecompilerImlGen_LookupReg(ppcImlGenContext, PPCREC_NAME_TEMPORARY_FPR0 + index, IMLRegFormat::F64);
}

// PS FMA: compute both lanes into temps, then commit with whole-register VE
// suppress (invalid on either lane restores both prev values).
static void emit_ppc_ps_fma_pair(ppcImlGenContext_t* ppcImlGenContext, uintptr_t fn,
	IMLReg fprD0, IMLReg fprD1,
	IMLReg fprA0, IMLReg fprC0, IMLReg fprB0,
	IMLReg fprA1, IMLReg fprC1, IMLReg fprB1)
{
	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprD0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprD1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	// Lane 0 → temp
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(fn, fprA0, fprC0, fprB0, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	// Lane 1 → temp
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(fn, fprA1, fprC1, fprB1, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	// Commit (may restore prev on either-lane VE suppress)
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprD0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprD1);
	// FPRF from ps0; stickies/FI from both lanes.
	// single_domain=true encoded as imm in r0 is not available; use a tiny wrapper.
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprD0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
}

IMLReg _GetFPRReg(ppcImlGenContext_t* ppcImlGenContext, uint32 regIndex, bool selectPS1)
{
	cemu_assert_debug(regIndex < 32);
	return PPCRecompilerImlGen_LookupReg(ppcImlGenContext, PPCREC_NAME_FPR_HALF + regIndex * 2 + (selectPS1 ? 1 : 0), IMLRegFormat::F64);
}

void PPRecompilerImmGen_roundToSinglePrecision(ppcImlGenContext_t* ppcImlGenContext, IMLReg fprRegister, bool flushDenormals=false)
{
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_ROUND_TO_SINGLE_PRECISION_BOTTOM, fprRegister);
	if( flushDenormals )
		assert_dbg();
}

bool PPCRecompilerImlGen_LFS_LFSU_LFD_LFDU(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withUpdate, bool isDouble)
{
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(opcode, frD, rA, imm);
	IMLReg gprRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	if (withUpdate)
	{
		// add imm to memory register
		cemu_assert_debug(rA != 0);
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_ADD, gprRegister, gprRegister, (sint32)imm);
		imm = 0; // set imm to 0 so we dont add it twice
	}
	DefinePS0(fpPs0, frD);
	if (isDouble)
	{
		// LFD/LFDU — high word may leak into ps1 if FPR is PS-write dirty.
		ppcImlGenContext->emitInst().make_fpr_r_memory(fpPs0, gprRegister, imm, PPCREC_FPR_LD_MODE_DOUBLE, true);
		if (ppcImlGenContext->PSE)
		{
			DefinePS1(fpPs1, frD);
			ppcImlGenContext->emitInst().make_call_imm(g_lfd_ps1_fr_fn[frD & 31], fpPs0, fpPs1, IMLREG_INVALID, fpPs1);
		}
	}
	else
	{
		// LFS/LFSU
		ppcImlGenContext->emitInst().make_fpr_r_memory(fpPs0, gprRegister, imm, PPCREC_FPR_LD_MODE_SINGLE, true);
		if( ppcImlGenContext->LSQE )
		{
			DefinePS1(fpPs1, frD);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fpPs1, fpPs0);
		}
	}
	return true;
}

bool PPCRecompilerImlGen_LFSX_LFSUX_LFDX_LFDUX(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withUpdate, bool isDouble)
{
	sint32 rA, frD, rB;
	PPC_OPC_TEMPL_X(opcode, frD, rA, rB);
	if( rA == 0 )
	{
		debugBreakpoint();
		return false;
	}
	// get memory gpr registers
	IMLReg gprRegister1 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	IMLReg gprRegister2 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rB);
	if (withUpdate)
		ppcImlGenContext->emitInst().make_r_r_r(PPCREC_IML_OP_ADD, gprRegister1, gprRegister1, gprRegister2);
	DefinePS0(fpPs0, frD);
	if (isDouble)
	{
		if (withUpdate)
			ppcImlGenContext->emitInst().make_fpr_r_memory(fpPs0, gprRegister1, 0, PPCREC_FPR_LD_MODE_DOUBLE, true);
		else
			ppcImlGenContext->emitInst().make_fpr_r_memory_indexed(fpPs0, gprRegister1, gprRegister2, PPCREC_FPR_LD_MODE_DOUBLE, true);
		if (ppcImlGenContext->PSE)
		{
			DefinePS1(fpPs1, frD);
			ppcImlGenContext->emitInst().make_call_imm(g_lfd_ps1_fr_fn[frD & 31], fpPs0, fpPs1, IMLREG_INVALID, fpPs1);
		}
	}
	else
	{
		if (withUpdate)
			ppcImlGenContext->emitInst().make_fpr_r_memory( fpPs0, gprRegister1, 0, PPCREC_FPR_LD_MODE_SINGLE, true);
		else
			ppcImlGenContext->emitInst().make_fpr_r_memory_indexed( fpPs0, gprRegister1, gprRegister2, PPCREC_FPR_LD_MODE_SINGLE, true);
		if( ppcImlGenContext->LSQE )
		{
			DefinePS1(fpPs1, frD);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fpPs1, fpPs0);
		}
	}
	return true;
}

bool PPCRecompilerImlGen_STFS_STFSU_STFD_STFDU(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withUpdate, bool isDouble)
{
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(opcode, frD, rA, imm);
	IMLReg gprRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	DefinePS0(fpPs0, frD);
	if (withUpdate)
	{
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_ADD, gprRegister, gprRegister, (sint32)imm);
		imm = 0;
	}
	if (isDouble)
		ppcImlGenContext->emitInst().make_fpr_memory_r(fpPs0, gprRegister, imm, PPCREC_FPR_ST_MODE_DOUBLE, true);
	else
		ppcImlGenContext->emitInst().make_fpr_memory_r(fpPs0, gprRegister, imm, PPCREC_FPR_ST_MODE_SINGLE, true);
	return true;
}

bool PPCRecompilerImlGen_STFSX_STFSUX_STFDX_STFDUX(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool hasUpdate, bool isDouble)
{
	sint32 rA, frS, rB;
	PPC_OPC_TEMPL_X(opcode, frS, rA, rB);
	if( rA == 0 )
	{
		debugBreakpoint();
		return false;
	}
	// get memory gpr registers
	IMLReg gprRegister1 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	IMLReg gprRegister2 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rB);
	if (hasUpdate)
	{
		ppcImlGenContext->emitInst().make_r_r_r(PPCREC_IML_OP_ADD, gprRegister1, gprRegister1, gprRegister2);
	}
	DefinePS0(fpPs0, frS);
	auto mode = isDouble ? PPCREC_FPR_ST_MODE_DOUBLE : PPCREC_FPR_ST_MODE_SINGLE;
	if( ppcImlGenContext->LSQE )
	{
		if (hasUpdate)
			ppcImlGenContext->emitInst().make_fpr_memory_r(fpPs0, gprRegister1, 0, mode, true);
		else
			ppcImlGenContext->emitInst().make_fpr_memory_r_indexed(fpPs0, gprRegister1, gprRegister2, 0, mode, true);
	}
	else
	{
		if (hasUpdate)
			ppcImlGenContext->emitInst().make_fpr_memory_r(fpPs0, gprRegister1, 0, mode, true);
		else
			ppcImlGenContext->emitInst().make_fpr_memory_r_indexed(fpPs0, gprRegister1, gprRegister2, 0, mode, true);
	}
	return true;
}

bool PPCRecompilerImlGen_STFIWX(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 rA, frS, rB;
	PPC_OPC_TEMPL_X(opcode, frS, rA, rB);
	DefinePS0(fpPs0, frS);
	IMLReg gprRegister1;
	IMLReg gprRegister2;
	if( rA != 0 )
	{
		gprRegister1 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
		gprRegister2 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rB);
	}
	else
	{
		// rA is not used
		gprRegister1 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rB);
		gprRegister2 = IMLREG_INVALID;
	}
	if( rA != 0 )
		ppcImlGenContext->emitInst().make_fpr_memory_r_indexed(fpPs0, gprRegister1, gprRegister2, 0, PPCREC_FPR_ST_MODE_UI32_FROM_PS0, true);
	else
		ppcImlGenContext->emitInst().make_fpr_memory_r(fpPs0, gprRegister1, 0, PPCREC_FPR_ST_MODE_UI32_FROM_PS0, true);
	return true;
}

bool PPCRecompilerImlGen_FADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadd, fprA, fprB, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompilerImlGen_FSUB(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fsub, fprA, fprB, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompilerImlGen_FMUL(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB_unused, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB_unused, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmul, fprA, fprC, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompilerImlGen_FDIV(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC_unused;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC_unused);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fdiv, fprA, fprB, IMLREG_INVALID, fprD);
	return true;
}

// PPC fmadd family: call the interpreter helpers so NaN selection (A→B→C),
// SNaN quieting, nmadd-does-not-negate-NaN, and FPSCR[VE] suppress match silicon.
// make_call_imm args are (frA, frC, frB) → A·C ± B.
bool PPCRecompilerImlGen_FMADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fmadd, fprD, fprA, fprC, fprB);
	return true;
}

bool PPCRecompilerImlGen_FMSUB(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fmsub, fprD, fprA, fprC, fprB);
	return true;
}

bool PPCRecompilerImlGen_FNMSUB(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fnmsub, fprD, fprA, fprC, fprB);
	return true;
}

bool PPCRecompilerImlGen_FNMADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fnmadd, fprD, fprA, fprC, fprB);
	return true;
}

#define PSE_CopyResultToPs1() 	if( ppcImlGenContext->PSE ) \
								{ \
									DefinePS1(fprDPS1, frD); \
									ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprDPS1, fprD); \
								}

bool PPCRecompilerImlGen_FMULS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB_unused, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB_unused, frC);

	DefinePS0(fprA, frA);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	// ppc_fmuls: 25-bit frC + ldexp product; bind for VE suppress.
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmuls, fprA, fprC, IMLREG_INVALID, fprD);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FDIVS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC_unused;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC_unused);
	PPC_ASSERT(frB==0);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	if( frB == frD && frA != frB )
	{
		DefineTempFPR(fprTemp, 0);
		// move frA to temporary register
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprTemp, fprA);
		// divide bottom double of temporary register by bottom double of frB
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_DIVIDE, fprTemp, fprB);
		// move result to frD
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprTemp);
		// adjust accuracy
		PPRecompilerImmGen_roundToSinglePrecision(ppcImlGenContext, fprD);
		PSE_CopyResultToPs1();
		return true;
	}
	// move frA to frD (if different register)
	if( frD != frA )
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprA);
	// subtract bottom double of frB from bottom double of frD
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_DIVIDE, fprD, fprB);
	// adjust accuracy
	PPRecompilerImmGen_roundToSinglePrecision(ppcImlGenContext, fprD);
	PSE_CopyResultToPs1();
	return true;
}

ATTR_MS_ABI double ppc_fadds(double a, double b);
ATTR_MS_ABI double ppc_fsubs(double a, double b);

bool PPCRecompilerImlGen_FADDS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	// Shared helper: single-domain result + FPSCR (FPRF/FI/stickies).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadds, fprA, fprB, IMLREG_INVALID, fprD);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FSUBS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fsubs, fprA, fprB, IMLREG_INVALID, fprD);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FMADDS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	// Single-domain helper applies 25-bit frC internally (Inf-from-HUGE tracking).
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fmadds, fprD, fprA, fprC, fprB);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FMSUBS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fmsubs, fprD, fprA, fprC, fprB);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FNMSUBS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fnmsubs, fprD, fprA, fprC, fprB);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FNMADDS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	emit_ppc_fma_call(ppcImlGenContext, (uintptr_t)ppc_fnmadds, fprD, fprA, fprC, fprB);
	PSE_CopyResultToPs1();
	return true;
}

ATTR_MS_ABI void ppc_fcmpu_fpscr(double a, double b);
ATTR_MS_ABI void ppc_fcmpo_fpscr(double a, double b);

bool PPCRecompilerImlGen_FCMPO(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, crfD, frA, frB);
	crfD >>= 2;
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpo_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_FCMPU(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, crfD, frA, frB);
	crfD >>= 2;
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpu_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_FMR(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, rA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, rA, frB);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprB);
	return true;
}

bool PPCRecompilerImlGen_FABS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	if( frD != frB )
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprB);
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_ABS, fprD);
	return true;
}

bool PPCRecompilerImlGen_FNABS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	if( frD != frB )
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprB);
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_NEGATIVE_ABS, fprD);
	return true;
}

bool PPCRecompilerImlGen_FRES(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fres, fprB, IMLREG_INVALID, IMLREG_INVALID, fprD);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FRSP(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	// Helper clears host FZ (denorm results) and honours VE on SNaN.
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_frsp, fprB, IMLREG_INVALID, IMLREG_INVALID, fprD);
	PSE_CopyResultToPs1();
	return true;
}

bool PPCRecompilerImlGen_FNEG(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	if( opcode&PPC_OPC_RC )
		return false;
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	if( frD != frB )
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprD, fprB);
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_NEGATE, fprD);
	return true;
}

bool PPCRecompilerImlGen_FSEL(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);
	DefinePS0(fprC, frC);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_fpr_r_r_r_r(PPCREC_IML_OP_FPR_SELECT, fprD, fprA, fprB, fprC);
	if (opcode & PPC_OPC_RC)
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_update_cr1_abi, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	return true;
}

bool PPCRecompilerImlGen_FRSQRTE(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(opcode, frD, frA, frB, frC);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_frsqrte, fprB, IMLREG_INVALID, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompilerImlGen_FCTIWZ(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	// Pack 0xFFF8000x high word + NaN/range/VE — not a bare fcvtzs.
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fctiwz, fprB, IMLREG_INVALID, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompilerImlGen_FCTIW(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	PPC_OPC_TEMPL_X(opcode, frD, frA, frB);
	DefinePS0(fprB, frB);
	DefinePS0(fprD, frD);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprD, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fctiw, fprB, IMLREG_INVALID, IMLREG_INVALID, fprD);
	return true;
}

bool PPCRecompiler_isUGQRValueKnown(ppcImlGenContext_t* ppcImlGenContext, sint32 gqrIndex, uint32& gqrValue);

void PPCRecompilerImlGen_ClampInteger(ppcImlGenContext_t* ppcImlGenContext, IMLReg reg, sint32 clampMin, sint32 clampMax)
{
	IMLReg regTmpCondBool = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 1);
	// min(reg, clampMax)
	ppcImlGenContext->emitInst().make_compare_s32(reg, clampMax, regTmpCondBool, IMLCondition::SIGNED_GT);
	ppcImlGenContext->emitInst().make_conditional_jump(regTmpCondBool, false); // condition needs to be inverted because we skip if the condition is true
	PPCIMLGen_CreateSegmentBranchedPath(*ppcImlGenContext, *ppcImlGenContext->currentBasicBlock,
		[&](ppcImlGenContext_t& genCtx)
		{
			/* branch not taken */
			genCtx.emitInst().make_r_s32(PPCREC_IML_OP_ASSIGN, reg, clampMax);
		}
	);
	// max(reg, clampMin)
	ppcImlGenContext->emitInst().make_compare_s32(reg, clampMin, regTmpCondBool, IMLCondition::SIGNED_LT);
	ppcImlGenContext->emitInst().make_conditional_jump(regTmpCondBool, false);
	PPCIMLGen_CreateSegmentBranchedPath(*ppcImlGenContext, *ppcImlGenContext->currentBasicBlock,
		[&](ppcImlGenContext_t& genCtx)
		{
			/* branch not taken */
			genCtx.emitInst().make_r_s32(PPCREC_IML_OP_ASSIGN, reg, clampMin);
		}
	);
}

void PPCRecompilerIMLGen_GetPSQScale(ppcImlGenContext_t* ppcImlGenContext, IMLReg gqrRegister, IMLReg fprRegScaleOut, bool isLoad)
{
	IMLReg gprTmp2 = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 2);
	// extract scale factor and sign extend it
	constexpr sint32 scaleBitWidth = 6;
	ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_LEFT_SHIFT, gprTmp2, gqrRegister, 32 - ((isLoad ? 24 : 8) + scaleBitWidth));
	ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_RIGHT_SHIFT_S, gprTmp2, gprTmp2, (32 - 23) - scaleBitWidth);
	ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_AND, gprTmp2, gprTmp2, 0x1FF<<23);
	if (isLoad)
		ppcImlGenContext->emitInst().make_r_r(PPCREC_IML_OP_NEG, gprTmp2, gprTmp2);
	ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_ADD, gprTmp2, gprTmp2, 0x7F<<23);
	// gprTmp2 now holds the scale float bits, bitcast to float
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_BITCAST_INT_TO_FLOAT, fprRegScaleOut, gprTmp2);
}

void PPCRecompilerImlGen_EmitPSQLoadCase(ppcImlGenContext_t* ppcImlGenContext, sint32 gqrIndex, Espresso::PSQ_LOAD_TYPE loadType, bool readPS1, IMLReg gprA, sint32 imm, IMLReg fprDPS0, IMLReg fprDPS1)
{
	if (loadType == Espresso::PSQ_LOAD_TYPE::TYPE_F32)
	{
		ppcImlGenContext->emitInst().make_fpr_r_memory(fprDPS0, gprA, imm, PPCREC_FPR_LD_MODE_SINGLE, true);
		if(readPS1)
		{
			ppcImlGenContext->emitInst().make_fpr_r_memory(fprDPS1, gprA, imm + 4, PPCREC_FPR_LD_MODE_SINGLE, true);
		}
	}
	if (loadType == Espresso::PSQ_LOAD_TYPE::TYPE_U16 || loadType == Espresso::PSQ_LOAD_TYPE::TYPE_S16)
	{
		// get scale factor
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg fprScaleReg = _GetFPRTemp(ppcImlGenContext, 2);
		PPCRecompilerIMLGen_GetPSQScale(ppcImlGenContext, gqrRegister, fprScaleReg, true);

		bool isSigned = (loadType == Espresso::PSQ_LOAD_TYPE::TYPE_S16);
		IMLReg gprTmp = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		ppcImlGenContext->emitInst().make_r_memory(gprTmp, gprA, imm, 16, isSigned, true);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_INT_TO_FLOAT, fprDPS0, gprTmp);

		ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprDPS0, fprDPS0, fprScaleReg);

		if(readPS1)
		{
			ppcImlGenContext->emitInst().make_r_memory(gprTmp, gprA, imm + 2, 16, isSigned, true);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_INT_TO_FLOAT, fprDPS1, gprTmp);
			ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprDPS1, fprDPS1, fprScaleReg);
		}
	}
	else if (loadType == Espresso::PSQ_LOAD_TYPE::TYPE_U8 || loadType == Espresso::PSQ_LOAD_TYPE::TYPE_S8)
	{
		// get scale factor
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg fprScaleReg = _GetFPRTemp(ppcImlGenContext, 2);
		PPCRecompilerIMLGen_GetPSQScale(ppcImlGenContext, gqrRegister, fprScaleReg, true);

		bool isSigned = (loadType == Espresso::PSQ_LOAD_TYPE::TYPE_S8);
		IMLReg gprTmp = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		ppcImlGenContext->emitInst().make_r_memory(gprTmp, gprA, imm, 8, isSigned, true);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_INT_TO_FLOAT, fprDPS0, gprTmp);
		ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprDPS0, fprDPS0, fprScaleReg);
		if(readPS1)
		{
			ppcImlGenContext->emitInst().make_r_memory(gprTmp, gprA, imm + 1, 8, isSigned, true);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_INT_TO_FLOAT, fprDPS1, gprTmp);
			ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprDPS1, fprDPS1, fprScaleReg);
		}
	}
}

// PSQ_L and PSQ_LU
bool PPCRecompilerImlGen_PSQ_L(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withUpdate)
{
	int rA, frD;
	uint32 immUnused;
	PPC_OPC_TEMPL_D_SImm(opcode, frD, rA, immUnused);
	sint32 gqrIndex = ((opcode >> 12) & 7);
	uint32 imm = opcode & 0xFFF;
	if (imm & 0x800)
		imm |= ~0xFFF;
	bool readPS1 = (opcode & 0x8000) == false;

	IMLReg gprA = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	DefinePS0(fprDPS0, frD);
	DefinePS1(fprDPS1, frD);
	if (!readPS1)
	{
		// if PS1 is not explicitly read then set it to 1.0
		ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_LOAD_ONE, fprDPS1);
	}
	if (withUpdate)
	{
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_ADD, gprA, gprA, (sint32)imm);
		imm = 0;
	}
	uint32 knownGQRValue = 0;
	if ( !PPCRecompiler_isUGQRValueKnown(ppcImlGenContext, gqrIndex, knownGQRValue) )
	{
		// generate complex dynamic handler when we dont know the GQR value ahead of time
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg loadTypeReg = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		// extract the load type from the GQR register
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_RIGHT_SHIFT_U, loadTypeReg, gqrRegister, 16);
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_AND, loadTypeReg, loadTypeReg, 0x7);
		IMLSegment* caseSegment[6];
		sint32 compareValues[6] = {0, 4, 5, 6, 7};
		PPCIMLGen_CreateSegmentBranchedPathMultiple(*ppcImlGenContext, *ppcImlGenContext->currentBasicBlock, caseSegment, loadTypeReg, compareValues, 5, 0);
		for (sint32 i=0; i<5; i++)
		{
			IMLRedirectInstOutput outputToCase(ppcImlGenContext, caseSegment[i]); // while this is in scope, instructions go to caseSegment[i]
			PPCRecompilerImlGen_EmitPSQLoadCase(ppcImlGenContext, gqrIndex, static_cast<Espresso::PSQ_LOAD_TYPE>(compareValues[i]), readPS1, gprA, imm, fprDPS0, fprDPS1);
			// create the case jump instructions here because we need to add it last
			caseSegment[i]->AppendInstruction()->make_jump();
		}
		return true;
	}

	Espresso::PSQ_LOAD_TYPE type = static_cast<Espresso::PSQ_LOAD_TYPE>((knownGQRValue >> 16) & 0x7);
	sint32 scale = (knownGQRValue >> 24) & 0x3F;
	cemu_assert_debug(scale == 0); // known GQR values always use a scale of 0 (1.0f)
	if (scale != 0)
		return false;

	if (type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED1 ||
		type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED2 ||
		type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED3)
	{
		return false;
	}

	PPCRecompilerImlGen_EmitPSQLoadCase(ppcImlGenContext, gqrIndex, type, readPS1, gprA, imm, fprDPS0, fprDPS1);
	return true;
}

void PPCRecompilerImlGen_EmitPSQStoreCase(ppcImlGenContext_t* ppcImlGenContext, sint32 gqrIndex, Espresso::PSQ_LOAD_TYPE storeType, bool storePS1, IMLReg gprA, sint32 imm, IMLReg fprDPS0, IMLReg fprDPS1)
{
	cemu_assert_debug(!storePS1 || fprDPS1.IsValid());
	if (storeType == Espresso::PSQ_LOAD_TYPE::TYPE_F32)
	{
		ppcImlGenContext->emitInst().make_fpr_memory_r(fprDPS0, gprA, imm, PPCREC_FPR_ST_MODE_SINGLE, true);
		if(storePS1)
		{
			ppcImlGenContext->emitInst().make_fpr_memory_r(fprDPS1, gprA, imm + 4, PPCREC_FPR_ST_MODE_SINGLE, true);
		}
	}
	else if (storeType == Espresso::PSQ_LOAD_TYPE::TYPE_U16 || storeType == Espresso::PSQ_LOAD_TYPE::TYPE_S16)
	{
		// get scale factor
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg fprScaleReg = _GetFPRTemp(ppcImlGenContext, 2);
		PPCRecompilerIMLGen_GetPSQScale(ppcImlGenContext, gqrRegister, fprScaleReg, false);

		bool isSigned = (storeType == Espresso::PSQ_LOAD_TYPE::TYPE_S16);
		IMLReg fprTmp = _GetFPRTemp(ppcImlGenContext, 0);

		IMLReg gprTmp = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprTmp, fprDPS0, fprScaleReg);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_FLOAT_TO_INT, gprTmp, fprTmp);

		if (isSigned)
			PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, -32768, 32767);
		else
			PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, 0, 65535);
		ppcImlGenContext->emitInst().make_memory_r(gprTmp, gprA, imm, 16, true);
		if(storePS1)
		{
			ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprTmp, fprDPS1, fprScaleReg);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_FLOAT_TO_INT, gprTmp, fprTmp);
			if (isSigned)
				PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, -32768, 32767);
			else
				PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, 0, 65535);
			ppcImlGenContext->emitInst().make_memory_r(gprTmp, gprA, imm + 2, 16, true);
		}
	}
	else if (storeType == Espresso::PSQ_LOAD_TYPE::TYPE_U8 || storeType == Espresso::PSQ_LOAD_TYPE::TYPE_S8)
	{
		// get scale factor
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg fprScaleReg = _GetFPRTemp(ppcImlGenContext, 2);
		PPCRecompilerIMLGen_GetPSQScale(ppcImlGenContext, gqrRegister, fprScaleReg, false);

		bool isSigned = (storeType == Espresso::PSQ_LOAD_TYPE::TYPE_S8);
		IMLReg fprTmp = _GetFPRTemp(ppcImlGenContext, 0);
		IMLReg gprTmp = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprTmp, fprDPS0, fprScaleReg);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_FLOAT_TO_INT, gprTmp, fprTmp);
		if (isSigned)
			PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, -128, 127);
		else
			PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, 0, 255);
		ppcImlGenContext->emitInst().make_memory_r(gprTmp, gprA, imm, 8, true);
		if(storePS1)
		{
			ppcImlGenContext->emitInst().make_fpr_r_r_r(PPCREC_IML_OP_FPR_MULTIPLY, fprTmp, fprDPS1, fprScaleReg);
			ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_FLOAT_TO_INT, gprTmp, fprTmp);
			if (isSigned)
				PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, -128, 127);
			else
				PPCRecompilerImlGen_ClampInteger(ppcImlGenContext, gprTmp, 0, 255);
			ppcImlGenContext->emitInst().make_memory_r(gprTmp, gprA, imm + 1, 8, true);
		}
	}
}

// PSQ_ST and PSQ_STU
bool PPCRecompilerImlGen_PSQ_ST(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withUpdate)
{
	int rA, frD;
	uint32 immUnused;
	PPC_OPC_TEMPL_D_SImm(opcode, frD, rA, immUnused);
	uint32 imm = opcode & 0xFFF;
	if (imm & 0x800)
		imm |= ~0xFFF;
	sint32 gqrIndex = ((opcode >> 12) & 7);
	bool storePS1 = (opcode & 0x8000) == false;

	IMLReg gprA = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_R0+rA);
	DefinePS0(fprDPS0, frD);
	IMLReg fprDPS1 = storePS1 ? _GetFPRRegPS1(ppcImlGenContext, frD) : IMLREG_INVALID;

	if (withUpdate)
	{
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_ADD, gprA, gprA, (sint32)imm);
		imm = 0;
	}

	uint32 gqrValue = 0;
	if ( !PPCRecompiler_isUGQRValueKnown(ppcImlGenContext, gqrIndex, gqrValue) )
	{
		// generate complex dynamic handler when we dont know the GQR value ahead of time
		IMLReg gqrRegister = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_SPR0 + SPR_UGQR0 + gqrIndex);
		IMLReg loadTypeReg = PPCRecompilerImlGen_loadRegister(ppcImlGenContext, PPCREC_NAME_TEMPORARY + 0);
		// extract the load type from the GQR register
		ppcImlGenContext->emitInst().make_r_r_s32(PPCREC_IML_OP_AND, loadTypeReg, gqrRegister, 0x7);

		IMLSegment* caseSegment[5];
		sint32 compareValues[5] = {0, 4, 5, 6, 7};
		PPCIMLGen_CreateSegmentBranchedPathMultiple(*ppcImlGenContext, *ppcImlGenContext->currentBasicBlock, caseSegment, loadTypeReg, compareValues, 5, 0);
		for (sint32 i=0; i<5; i++)
		{
			IMLRedirectInstOutput outputToCase(ppcImlGenContext, caseSegment[i]); // while this is in scope, instructions go to caseSegment[i]
			PPCRecompilerImlGen_EmitPSQStoreCase(ppcImlGenContext, gqrIndex, static_cast<Espresso::PSQ_LOAD_TYPE>(compareValues[i]), storePS1, gprA, imm, fprDPS0, fprDPS1);
			ppcImlGenContext->emitInst().make_jump(); // finalize case
		}
		return true;
	}

	Espresso::PSQ_LOAD_TYPE type = static_cast<Espresso::PSQ_LOAD_TYPE>((gqrValue >> 0) & 0x7);
	sint32 scale = (gqrValue >> 24) & 0x3F;
	cemu_assert_debug(scale == 0); // known GQR values always use a scale of 0 (1.0f)

	if (type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED1 ||
		type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED2 ||
		type == Espresso::PSQ_LOAD_TYPE::TYPE_UNUSED3)
	{
		return false;
	}

	PPCRecompilerImlGen_EmitPSQStoreCase(ppcImlGenContext, gqrIndex, type, storePS1, gprA, imm, fprDPS0, fprDPS1);
	return true;
}

// PS_MULS0 and PS_MULS1
bool PPCRecompilerImlGen_PS_MULSX(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool isVariant1)
{
	sint32 frD, frA, frC;
	frC = (opcode>>6)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePSX(fprC, frC, isVariant1);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

		// Shared raw frC; whole-register VE suppress like PS FMA.
	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmuls, fprAps0, fprC, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmuls, fprAps1, fprC, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

// PS_MADDS0 and PS_MADDS1
bool PPCRecompilerImlGen_PS_MADDSX(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool isVariant1)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePSX(fprC, frC, isVariant1);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	// Shared frC lane; whole-register VE suppress via pair helper.
	emit_ppc_ps_fma_pair(ppcImlGenContext, (uintptr_t)ppc_fmadds,
		fprDps0, fprDps1,
		fprAps0, fprC, fprBps0,
		fprAps1, fprC, fprBps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_ADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);

	// NaN/VE whole-reg + single pack — same helpers as the interpreter.
	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadd, fprAps0, fprBps0, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadd, fprAps1, fprBps1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_SUB(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);

	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fsub, fprAps0, fprBps0, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fsub, fprAps1, fprBps1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MUL(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frC;
	frC = (opcode >> 6) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	frD = (opcode >> 21) & 0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprCps0, frC);
	DefinePS1(fprCps1, frC);

	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmuls, fprAps0, fprCps0, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fmuls, fprAps1, fprCps1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_DIV(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	// Must use ppc_fdiv (VE/ZE suppress, VX*, ZX, OX) — bare FPR_DIVIDE skips FPSCR
	// and breaks check_ps_noresult. Same two-lane pattern as PS_ADD.
	sint32 frD, frA, frB;
	frB = (opcode >> 11) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	frD = (opcode >> 21) & 0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);

	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fdiv, fprAps0, fprBps0, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fdiv, fprAps1, fprBps1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprCps0, frC);
	DefinePS1(fprCps1, frC);

	emit_ppc_ps_fma_pair(ppcImlGenContext, (uintptr_t)ppc_fmadds,
		fprDps0, fprDps1,
		fprAps0, fprCps0, fprBps0,
		fprAps1, fprCps1, fprBps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_NMADD(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprCps0, frC);
	DefinePS1(fprCps1, frC);

	// Splatoon denormal flush for this family is a separate accuracy item.
	emit_ppc_ps_fma_pair(ppcImlGenContext, (uintptr_t)ppc_fnmadds,
		fprDps0, fprDps1,
		fprAps0, fprCps0, fprBps0,
		fprAps1, fprCps1, fprBps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

// PS_MSUB and PS_NMSUB
bool PPCRecompilerImlGen_PS_MSUB(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode, bool withNegative)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprCps0, frC);
	DefinePS1(fprCps1, frC);

	const uintptr_t fn = withNegative ? (uintptr_t)ppc_fnmsubs : (uintptr_t)ppc_fmsubs;
	emit_ppc_ps_fma_pair(ppcImlGenContext, fn,
		fprDps0, fprDps1,
		fprAps0, fprCps0, fprBps0,
		fprAps1, fprCps1, fprBps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_SUM0(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	// Interpreter path owns VE whole-reg semantics for sum; fall back if complex.
	// Emit call sequence matching the interpreter: sum = pack(fadd A0,B1); copy = quantize C1.
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprBps1, frB);
	DefinePS1(fprCps1, frC);
	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	// Full fidelity: always use interpreter for sum0 (VE whole-reg + copy) —
	// returning false would deoptimize the block. Instead mirror interpreter:
	// bind prev0, fadd+pack, if suppressed leave both (handled inside helpers poorly).
	// Simplest correct path matching interp: call_imm per slot with was_suppressed
	// is awkward in IML. Use the same two-lane suppress machinery with only lane0 arithmetic.
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadd, fprAps0, fprBps1, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	// Fake lane1 note with suppress already set if sum invalid — re-note copies flag.
	// Force note_i=1 with a second note that re-reads s_fma_suppressed (still set if VE).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, fprCps1, IMLREG_INVALID, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	// If suppressed, Dps1 was already quantized into itself wrongly — commit both via prev.
	// Re-commit Dps1 from prev when suppressed: use commit_lane(prev1, Dps1).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprDps1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_SUM1(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	DefinePS0(fprAps0, frA);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprCps0, frC);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	// Lane0 = copy (never invalid). bind_dest clears s_fma_suppressed so the note is false.
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fadd, fprAps0, fprBps1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_pack_arith, fprT1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, fprCps0, IMLREG_INVALID, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprDps0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_NEG(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frB;
	frB = (opcode>>11)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	if (frB != frD)
	{
		// copy
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprDps0, fprBps0);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprDps1, fprBps1);
	}
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_NEGATE, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_NEGATE, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_ABS(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frB;
	frB = (opcode>>11)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprDps0, fprBps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprDps1, fprBps1);

	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_ABS, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r(PPCREC_IML_OP_FPR_ABS, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_RES(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frB;
	frB = (opcode>>11)&0x1F;
	frD = (opcode>>21)&0x1F;
	//hCPU->fpr[frD].fp0 = (float)(1.0f / (float)hCPU->fpr[frB].fp0);
	//hCPU->fpr[frD].fp1 = (float)(1.0f / (float)hCPU->fpr[frB].fp1);

	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fres, fprBps0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fres, fprBps1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_single, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_RSQRTE(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frB;
	frB = (opcode>>11)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	DefineTempFPR(fprT0, 0);
	DefineTempFPR(fprT1, 1);
	DefineTempFPR(fprPrev0, 2);
	DefineTempFPR(fprPrev1, 3);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev0, fprDps0);
	ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, fprPrev1, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_reset_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_begin, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_frsqrte, fprBps0, IMLREG_INVALID, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fold_estimate, fprBps0, fprT0, IMLREG_INVALID, fprT0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fma_bind_dest, fprPrev1, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_frsqrte, fprBps1, IMLREG_INVALID, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fold_estimate, fprBps1, fprT1, IMLREG_INVALID, fprT1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_note_suppress, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev0, fprT0, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_fma_commit_lane, fprPrev1, fprT1, IMLREG_INVALID, fprDps1);
	// rsqrtte is double-domain FPRF (ps0).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_defer_end_double, fprDps0, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	if (opcode & PPC_OPC_RC)
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_update_cr1_abi, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MR(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frB;
	frB = (opcode>>11)&0x1F;
	frD = (opcode>>21)&0x1F;
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);
	// Always quantize (including frD==frB excess-precision cases).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, fprBps0, IMLREG_INVALID, IMLREG_INVALID, fprDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, fprBps1, IMLREG_INVALID, IMLREG_INVALID, fprDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_SEL(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB, frC;
	frC = (opcode>>6)&0x1F;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS0(fprAps0, frA);
	DefinePS1(fprAps1, frA);
	DefinePS0(fprBps0, frB);
	DefinePS1(fprBps1, frB);
	DefinePS0(fprCps0, frC);
	DefinePS1(fprCps1, frC);
	DefinePS0(fprDps0, frD);
	DefinePS1(fprDps1, frD);

	ppcImlGenContext->emitInst().make_fpr_r_r_r_r(PPCREC_IML_OP_FPR_SELECT, fprDps0, fprAps0, fprBps0, fprCps0);
	ppcImlGenContext->emitInst().make_fpr_r_r_r_r(PPCREC_IML_OP_FPR_SELECT, fprDps1, fprAps1, fprBps1, fprCps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);
	if (opcode & PPC_OPC_RC)
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fpscr_update_cr1_abi, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MERGE00(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;
	DefinePS0(frpAps0, frA);
	DefinePS0(frpBps0, frB);
	DefinePS0(frpDps0, frD);
	DefinePS1(frpDps1, frD);
	// Dest slot0 RN, slot1 truncate (suite excess-range merge).
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize_tz, frpBps0, IMLREG_INVALID, IMLREG_INVALID, frpDps1);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, frpAps0, IMLREG_INVALID, IMLREG_INVALID, frpDps0);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MERGE01(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;
	DefinePS0(frpAps0, frA);
	DefinePS1(frpBps1, frB);
	DefinePS0(frpDps0, frD);
	DefinePS1(frpDps1, frD);

	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, frpAps0, IMLREG_INVALID, IMLREG_INVALID, frpDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize_tz, frpBps1, IMLREG_INVALID, IMLREG_INVALID, frpDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MERGE10(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS1(frpAps1, frA);
	DefinePS0(frpBps0, frB);
	DefinePS0(frpDps0, frD);
	DefinePS1(frpDps1, frD);

	if (frD != frB)
	{
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, frpAps1, IMLREG_INVALID, IMLREG_INVALID, frpDps0);
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize_tz, frpBps0, IMLREG_INVALID, IMLREG_INVALID, frpDps1);
	}
	else
	{
		DefineTempFPR(frpTemp, 0);
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize_tz, frpBps0, IMLREG_INVALID, IMLREG_INVALID, frpTemp);
		ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, frpAps1, IMLREG_INVALID, IMLREG_INVALID, frpDps0);
		ppcImlGenContext->emitInst().make_fpr_r_r(PPCREC_IML_OP_FPR_ASSIGN, frpDps1, frpTemp);
	}
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_MERGE11(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 frD, frA, frB;
	frB = (opcode>>11)&0x1F;
	frA = (opcode>>16)&0x1F;
	frD = (opcode>>21)&0x1F;

	DefinePS1(frpAps1, frA);
	DefinePS1(frpBps1, frB);
	DefinePS0(frpDps0, frD);
	DefinePS1(frpDps1, frD);

	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize, frpAps1, IMLREG_INVALID, IMLREG_INVALID, frpDps0);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_ps_quantize_tz, frpBps1, IMLREG_INVALID, IMLREG_INVALID, frpDps1);
	ppcImlGenContext->emitInst().make_call_imm(g_note_ps_write_fr_fn[frD & 31], IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID, IMLREG_INVALID);

	return true;
}

bool PPCRecompilerImlGen_PS_CMPO0(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	frB = (opcode >> 11) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	crfD = (opcode >> 23) & 0x7;

	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpo_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);
	return true;
}

bool PPCRecompilerImlGen_PS_CMPU0(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	frB = (opcode >> 11) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	crfD = (opcode >> 23) & 0x7;

	DefinePS0(fprA, frA);
	DefinePS0(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpu_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);
	return true;
}

bool PPCRecompilerImlGen_PS_CMPU1(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	frB = (opcode >> 11) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	crfD = (opcode >> 23) & 0x7;

	DefinePS1(fprA, frA);
	DefinePS1(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpu_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);
	return true;
}

bool PPCRecompilerImlGen_PS_CMPO1(ppcImlGenContext_t* ppcImlGenContext, uint32 opcode)
{
	sint32 crfD, frA, frB;
	frB = (opcode >> 11) & 0x1F;
	frA = (opcode >> 16) & 0x1F;
	crfD = (opcode >> 23) & 0x7;

	DefinePS1(fprA, frA);
	DefinePS1(fprB, frB);

	IMLReg crBitRegLT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_LT);
	IMLReg crBitRegGT = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_GT);
	IMLReg crBitRegEQ = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_EQ);
	IMLReg crBitRegSO = _GetRegCR(ppcImlGenContext, crfD, Espresso::CR_BIT::CR_BIT_INDEX_SO);

	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegLT, IMLCondition::UNORDERED_LT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegGT, IMLCondition::UNORDERED_GT);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegEQ, IMLCondition::UNORDERED_EQ);
	ppcImlGenContext->emitInst().make_fpr_compare(fprA, fprB, crBitRegSO, IMLCondition::UNORDERED_U);
	ppcImlGenContext->emitInst().make_call_imm((uintptr_t)ppc_fcmpo_fpscr, fprA, fprB, IMLREG_INVALID, IMLREG_INVALID);
	return true;
}

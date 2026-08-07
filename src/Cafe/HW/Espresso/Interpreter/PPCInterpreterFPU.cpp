#include <cfenv>
#include <cfloat>
#include <arm_acle.h>
#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

#include <cmath>
#include <math.h>
#include <cstring>

// floating point utility

#include <limits>
#include <array>

const int ieee_double_e_bits = 11; // exponent bits
const int ieee_double_m_bits = 52; // mantissa bits

const int espresso_frsqrte_i_bits = 5; // index bits (the highest bit is the LSB of the exponent)

typedef struct
{
	uint32 offset;
	uint32 step;
}espresso_frsqrte_entry_t;

espresso_frsqrte_entry_t frsqrteLookupTable[32] =
{
	{0x1a7e800, 0x568},{0x17cb800, 0x4f3},{0x1552800, 0x48d},{0x130c000, 0x435},
	{0x10f2000, 0x3e7},{0xeff000, 0x3a2},{0xd2e000, 0x365},{0xb7c000, 0x32e},
	{0x9e5000, 0x2fc},{0x867000, 0x2d0},{0x6ff000, 0x2a8},{0x5ab800, 0x283},
	{0x46a000, 0x261},{0x339800, 0x243},{0x218800, 0x226},{0x105800, 0x20b},
	{0x3ffa000, 0x7a4},{0x3c29000, 0x700},{0x38aa000, 0x670},{0x3572000, 0x5f2},
	{0x3279000, 0x584},{0x2fb7000, 0x524},{0x2d26000, 0x4cc},{0x2ac0000, 0x47e},
	{0x2881000, 0x43a},{0x2665000, 0x3fa},{0x2468000, 0x3c2},{0x2287000, 0x38e},
	{0x20c1000, 0x35e},{0x1f12000, 0x332},{0x1d79000, 0x30a},{0x1bf4000, 0x2e6},
};

ATTR_MS_ABI double frsqrte_espresso(double input)
{
	unsigned long long x = *(unsigned long long*)&input;

	// 0.0 and -0.0
	if ((x << 1) == 0)
	{
		// result is inf or -inf
		x &= ~0x7FFFFFFFFFFFFFFF;
		x |= 0x7FF0000000000000;
		return *(double*)&x;
	}
	// get exponent
	uint32 e = (x >> ieee_double_m_bits) & ((1ull << ieee_double_e_bits) - 1ull);
	// NaN or INF
	if (e == 0x7FF)
	{
		if ((x&((1ull << ieee_double_m_bits) - 1)) == 0)
		{
			// negative INF returns +NaN
			if ((sint64)x < 0)
			{
				x = 0x7FF8000000000000;
				return *(double*)&x;
			}
			// positive INF returns +0.0
			return 0.0;
		}
		// result is NaN with same sign and same mantissa (todo: verify)
		return *(double*)&x;
	}
	// negative number (other than -0.0)
	if ((sint64)x < 0)
	{
		// result is positive NaN
		x = 0x7FF8000000000000;
		return *(double*)&x;
	}
	// todo: handle denormals

	// get index (lsb of exponent, remaining bits of mantissa)
	uint32 idx = (x >> (ieee_double_m_bits - espresso_frsqrte_i_bits + 1ull))&((1 << espresso_frsqrte_i_bits) - 1);
	// get step multiplier
	uint32 stepMul = (x >> (ieee_double_m_bits - espresso_frsqrte_i_bits + 1 - 11))&((1 << 11) - 1);

	sint32 sum = frsqrteLookupTable[idx].offset - frsqrteLookupTable[idx].step * stepMul;

	e = 1023 - ((e - 1021) >> 1);
	x &= ~(((1ull << ieee_double_e_bits) - 1ull) << ieee_double_m_bits);
	x |= ((unsigned long long)e << ieee_double_m_bits);

	x &= ~((1ull << ieee_double_m_bits) - 1ull);
	x += ((unsigned long long)sum << 26ull);

	return *(double*)&x;
}

const int espresso_fres_i_bits = 5; // index bits
const int espresso_fres_s_bits = 10; // step multiplier bits

typedef struct
{
	uint32 offset;
	uint32 step;
}espresso_fres_entry_t;

espresso_fres_entry_t fresLookupTable[32] =
{
	// table calculated by fres_gen_table()
	{0x7ff800, 0x3e1},	{0x783800, 0x3a7},	{0x70ea00, 0x371},	{0x6a0800, 0x340},
	{0x638800, 0x313},	{0x5d6200, 0x2ea},	{0x579000, 0x2c4},	{0x520800, 0x2a0},
	{0x4cc800, 0x27f},	{0x47ca00, 0x261},	{0x430800, 0x245},	{0x3e8000, 0x22a},
	{0x3a2c00, 0x212},	{0x360800, 0x1fb},	{0x321400, 0x1e5},	{0x2e4a00, 0x1d1},
	{0x2aa800, 0x1be},	{0x272c00, 0x1ac},	{0x23d600, 0x19b},	{0x209e00, 0x18b},
	{0x1d8800, 0x17c},	{0x1a9000, 0x16e},	{0x17ae00, 0x15b},	{0x14f800, 0x15b},
	{0x124400, 0x143},	{0xfbe00, 0x143},	{0xd3800, 0x12d},	{0xade00, 0x12d},
	{0x88400, 0x11a},	{0x65000, 0x11a},	{0x41c00, 0x108},	{0x20c00, 0x106}
};

ATTR_MS_ABI double roundTo25BitAccuracy(double d)
{
	// Truncate the IEEE-754 double mantissa to 25 bits, with round-to-nearest via the
	// sticky next bit (Espresso single-precision multiply product factor for frC).
	// Round-up of DBL_MAX overflows to Inf — that is intentional: `1 * HUGE + -HUGE`
	// becomes +Inf with OX, while `HUGE * 1 + -HUGE` stays 0 (25-bit is frC only).
	// `0 * HUGE + 1` is handled separately: Inf-from-25-bit is not a true Inf operand.
	uint64 v = *(uint64*)&d;
	if ((v & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
		return d;

	const uint64 rounded = (v & 0xFFFFFFFFF8000000ULL) + (v & 0x8000000ULL);
	return *(double*)&rounded;
}

// Like roundTo25BitAccuracy, but reports whether a finite input overflowed to Inf.
static inline double roundTo25BitAccuracyEx(double d, bool* outOverflowToInf)
{
	if (outOverflowToInf)
		*outOverflowToInf = false;
	const uint64 vIn = *(const uint64*)&d;
	const bool wasFinite = ((vIn & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL);
	const double r = roundTo25BitAccuracy(d);
	if (outOverflowToInf && wasFinite)
	{
		const uint64 vOut = *(const uint64*)&r;
		*outOverflowToInf = ((vOut & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL);
	}
	return r;
}

// --- PowerPC fused multiply-add specials (ppc750cl.s, Espresso silicon) ---
// NaN selection order is frA → frB → frC (not the A→C→B PEM order). SNaNs are
// quieted. Generated invalids (0·∞, ∞−∞) yield the default QNaN 0x7FF8… .
// For nmadd/nmsub the outer negation must NOT flip the sign of a selected NaN.

static constexpr uint64 kPpcDefaultQNaN = 0x7FF8000000000000ULL;

static inline bool ppc_bits_is_nan(uint64 x)
{
	return ((x & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
		&& ((x & 0x000FFFFFFFFFFFFFULL) != 0);
}

static inline bool ppc_bits_is_inf(uint64 x)
{
	return (x & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}

static inline bool ppc_bits_is_zero(uint64 x)
{
	return (x & 0x7FFFFFFFFFFFFFFFULL) == 0;
}

static inline uint64 ppc_quiet_nan(uint64 x)
{
	return x | 0x0008000000000000ULL;
}

// SNaN: exp all-ones, frac nonzero, quiet bit (MSB of frac) clear.
static inline bool ppc_bits_is_snan(uint64 x)
{
	return ppc_bits_is_nan(x) && ((x & 0x0008000000000000ULL) == 0);
}

// Special-case result class for VE handling:
//  0 = not special — use std::fma
//  1 = write *out (QNaN-only selection; no invalid exception → VE does not suppress)
//  2 = invalid (SNaN operand and/or 0·∞ / ∞−∞) — VE suppresses frD write
// cOverflowToInf: frC became Inf only via 25-bit round of a finite HUGE (not a
// true Inf operand). 0·that must not raise VXIMZ (suite: 0 * HUGE_VAL + 1).
static int ppc_fmadd_try_special(double a, double b, double c, bool isMsub, double* out,
	bool cOverflowToInf = false)
{
	const uint64 ua = *(uint64*)&a;
	const uint64 ub = *(uint64*)&b;
	const uint64 uc = *(uint64*)&c;

	const bool snanA = ppc_bits_is_snan(ua);
	const bool snanB = ppc_bits_is_snan(ub);
	const bool snanC = ppc_bits_is_snan(uc);
	const bool anySNaN = snanA || snanB || snanC;
	const bool nanA = ppc_bits_is_nan(ua);
	const bool nanB = ppc_bits_is_nan(ub);
	const bool nanC = ppc_bits_is_nan(uc);

	// NaN selection order frA → frB → frC (Espresso / ppc750cl.s).
	if (nanA || nanB || nanC)
	{
		uint64 selected;
		if (nanA)
			selected = ua;
		else if (nanB)
			selected = ub;
		else
			selected = uc;
		const uint64 r = ppc_quiet_nan(selected);
		*out = *(double*)&r;
		// SNaN anywhere signals invalid even if a QNaN was selected first.
		return anySNaN ? 2 : 1;
	}

	// 0 · ∞ or ∞ · 0 → VXIMZ default QNaN.
	// Inf that only exists because 25-bit rounded a finite HUGE is not a true Inf
	// operand: skip the invalid here (caller rewrites 0·overflowInf as 0·1).
	if (ppc_bits_is_zero(ua) && ppc_bits_is_inf(uc) && !cOverflowToInf)
	{
		*out = *(double*)&kPpcDefaultQNaN;
		return 2;
	}
	if (ppc_bits_is_inf(ua) && ppc_bits_is_zero(uc))
	{
		*out = *(double*)&kPpcDefaultQNaN;
		return 2;
	}
	if (ppc_bits_is_zero(ua) && ppc_bits_is_inf(uc) && cOverflowToInf)
		return 0; // numeric path will rewrite c

	// ∞ · finite ± ∞ of opposite sign → VXISI default QNaN
	if ((ppc_bits_is_inf(ua) || ppc_bits_is_inf(uc)) && ppc_bits_is_inf(ub))
	{
		const uint64 prodSign = (ua ^ uc) & 0x8000000000000000ULL;
		uint64 addSign = ub & 0x8000000000000000ULL;
		if (isMsub)
			addSign ^= 0x8000000000000000ULL;
		if (prodSign != addSign)
		{
			*out = *(double*)&kPpcDefaultQNaN;
			return 2;
		}
	}

	return 0;
}

// Prior frD + VE, set by ppc_fma_bind_dest before each helper call so both the
// interpreter and the recompiler can honour FPSCR[VE] result suppression.
static thread_local double s_fma_prev = 0.0;
static thread_local bool s_fma_ve = false;
static thread_local bool s_fma_suppressed = false;

ATTR_MS_ABI void ppc_fma_bind_dest(double prevFrD)
{
	s_fma_prev = prevFrD;
	s_fma_suppressed = false;
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	s_fma_ve = hCPU && (hCPU->fpscr & FPSCR_VE);
}

ATTR_MS_ABI bool ppc_fma_was_suppressed()
{
	return s_fma_suppressed;
}

// PS FMA whole-register VE suppress: if either lane hits invalid with VE, neither
// lane is written (suite: mixed SNaN + overflow → check_ps_noresult).
static thread_local bool s_ps_fma_suppress0 = false;
static thread_local bool s_ps_fma_suppress1 = false;
static thread_local int s_ps_fma_note_i = 0;

ATTR_MS_ABI void ppc_ps_fma_reset_suppress()
{
	s_ps_fma_suppress0 = false;
	s_ps_fma_suppress1 = false;
	s_ps_fma_note_i = 0;
}

ATTR_MS_ABI void ppc_ps_fma_note_suppress()
{
	const bool s = s_fma_suppressed;
	if (s_ps_fma_note_i == 0)
		s_ps_fma_suppress0 = s;
	else
		s_ps_fma_suppress1 = s;
	s_ps_fma_note_i++;
}

ATTR_MS_ABI double ppc_ps_fma_commit_lane(double prev, double computed)
{
	if (s_ps_fma_suppress0 || s_ps_fma_suppress1)
		return prev;
	return computed;
}

static inline double ppc_fma_finish_special(int kind, double specialResult)
{
	// kind 2 + VE → leave destination unchanged (suite check_fpu_noresult).
	if (kind == 2 && s_fma_ve)
	{
		s_fma_suppressed = true;
		return s_fma_prev;
	}
	s_fma_suppressed = false;
	return specialResult;
}

// Double-precision fma with host FPCR.FZ briefly cleared — FZ disturbs tininess
// edges (suite: min_normal*min_normal + -min_normal).
static inline double ppc_fma_double_nofz(double a, double c, double b)
{
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
	{
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
		const double r = std::fma(a, c, b);
		__arm_wsr64("fpcr", fpcr);
		return r;
	}
	return std::fma(a, c, b);
}

static inline float ppc_bits_to_f32(uint32 bits)
{
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

static inline uint32 ppc_f32_to_bits(float f)
{
	uint32 bits;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

ATTR_MS_ABI double ppc_fmadd(double a, double c, double b)
{
	double r;
	const int kind = ppc_fmadd_try_special(a, b, c, false, &r);
	if (kind != 0)
		return ppc_fma_finish_special(kind, r);
	s_fma_suppressed = false;
	return ppc_fma_double_nofz(a, c, b);
}

ATTR_MS_ABI double ppc_fmsub(double a, double c, double b)
{
	double r;
	const int kind = ppc_fmadd_try_special(a, b, c, true, &r);
	if (kind != 0)
		return ppc_fma_finish_special(kind, r);
	s_fma_suppressed = false;
	return ppc_fma_double_nofz(a, c, -b);
}

ATTR_MS_ABI double ppc_fnmadd(double a, double c, double b)
{
	double r;
	// Selected/generated NaN is not sign-flipped (suite fnmadd NaN cases use f10/f12 as-is).
	const int kind = ppc_fmadd_try_special(a, b, c, false, &r);
	if (kind != 0)
		return ppc_fma_finish_special(kind, r);
	s_fma_suppressed = false;
	return -ppc_fma_double_nofz(a, c, b);
}

ATTR_MS_ABI double ppc_fnmsub(double a, double c, double b)
{
	double r;
	const int kind = ppc_fmadd_try_special(a, b, c, true, &r);
	if (kind != 0)
		return ppc_fma_finish_special(kind, r);
	s_fma_suppressed = false;
	return -ppc_fma_double_nofz(a, c, -b);
}

// Pack a double FMA intermediate into the single-precision FPR layout.
// In-range: ConvertToSingleNoFTZ (suite denorm sticky: min_denorm*1.5 − min_denorm_d
// → 0x1, not IEEE RN 0x2). Out-of-range finite: ±Inf (suite: 1*0.5−HUGE → −Inf);
// plain ConvertToSingleNoFTZ would leave HUGE_VALF.
static inline double ppc_fma_result_to_single(double r)
{
	if (std::isfinite(r))
	{
		// max finite single as double (HUGE_VALF).
		constexpr double kMaxSingle = 0x1.fffffep+127;
		if (r > kMaxSingle)
			r = std::numeric_limits<double>::infinity();
		else if (r < -kMaxSingle)
			r = -std::numeric_limits<double>::infinity();
		else
		{
			const uint32 sr = ConvertToSingleNoFTZ(*(const uint64*)&r);
			const uint64 dr = ConvertToDoubleNoFTZ(sr);
			return *(double*)&dr;
		}
	}
	const uint32 sr = ConvertToSingleNoFTZ(*(const uint64*)&r);
	const uint64 dr = ConvertToDoubleNoFTZ(sr);
	return *(double*)&dr;
}

// Single-precision multiply-add domain.
// cIn is the raw frC; 25-bit rounding is applied here so we can tell true Inf
// from Inf-from-HUGE (needed for 0·HUGE vs 1·HUGE + −HUGE vs 0.5·HUGE + −HUGE).
static inline double ppc_fma_single_domain(double a, double cIn, double b, bool isMsub, bool negate)
{
	bool cOverflowToInf = false;
	double c = roundTo25BitAccuracyEx(cIn, &cOverflowToInf);

	double special;
	const int kind = ppc_fmadd_try_special(a, b, c, isMsub, &special, cOverflowToInf);
	if (kind != 0)
		return ppc_fma_finish_special(kind, special);
	s_fma_suppressed = false;

	if (isMsub)
		b = -b;

	double r;
	if (cOverflowToInf)
	{
		// 25-bit round of DBL_MAX is 2^1024, which IEEE-packs as Inf. Multiplying
		// as Inf first is wrong: 0.5·Inf is Inf, but 0.5·2^1024 = 2^1023 is finite.
		// Suite: 1·HUGE+−HUGE → +Inf, 0.5·HUGE+−HUGE → −Inf, 0·HUGE+1 → 1.
		// ldexp(|a|, 1024) keeps that distinction (0 → 0, 0.5 → 2^1023, 1 → Inf).
		const double mag = std::ldexp(std::fabs(a), 1024);
		const double prod = (std::signbit(a) != std::signbit(c)) ? -mag : mag;
		r = prod + b;
		r = ppc_fma_result_to_single(r);
	}
	else
	{
		const uint64 ua = *(const uint64*)&a;
		const uint64 uc = *(const uint64*)&c;
		const uint64 ub = *(const uint64*)&b;
		const uint32 sa = ConvertToSingleNoFTZ(ua);
		const uint32 sc = ConvertToSingleNoFTZ(uc);
		const uint32 sb = ConvertToSingleNoFTZ(ub);
		const float fa = ppc_bits_to_f32(sa);
		const float fc = ppc_bits_to_f32(sc);
		const float fb = ppc_bits_to_f32(sb);

		// Finite double that becomes Inf as f32, or excess precision → double fma
		// then IEEE-round to single (not ConvertToSingleNoFTZ truncate).
		const bool overflowed =
			(std::isfinite(a) && !std::isfinite(fa)) ||
			(std::isfinite(c) && !std::isfinite(fc)) ||
			(std::isfinite(b) && !std::isfinite(fb));
		const bool excessPrecision =
			ConvertToDoubleNoFTZ(sa) != ua ||
			ConvertToDoubleNoFTZ(sc) != uc ||
			ConvertToDoubleNoFTZ(sb) != ub;

		if (overflowed || excessPrecision)
		{
			r = ppc_fma_double_nofz(a, c, b);
			r = ppc_fma_result_to_single(r);
		}
		else
		{
			// Exact f32 operands: fmaf keeps denormal sticky (suite 0x7F000001).
			const uint64 fpcr = __arm_rsr64("fpcr");
			if (fpcr & (1ull << 24))
				__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
			const uint32 sr = ppc_f32_to_bits(std::fmaf(fa, fc, fb));
			if (fpcr & (1ull << 24))
				__arm_wsr64("fpcr", fpcr);
			const uint64 dr = ConvertToDoubleNoFTZ(sr);
			r = *(double*)&dr;
		}
	}
	return negate ? -r : r;
}

ATTR_MS_ABI double ppc_fmadds(double a, double c, double b)
{
	// c is raw frC — 25-bit applied inside.
	return ppc_fma_single_domain(a, c, b, false, false);
}
ATTR_MS_ABI double ppc_fmsubs(double a, double c, double b)
{
	return ppc_fma_single_domain(a, c, b, true, false);
}
ATTR_MS_ABI double ppc_fnmadds(double a, double c, double b)
{
	return ppc_fma_single_domain(a, c, b, false, true);
}
ATTR_MS_ABI double ppc_fnmsubs(double a, double c, double b)
{
	return ppc_fma_single_domain(a, c, b, true, true);
}

ATTR_MS_ABI double fres_espresso(double input)
{
	// based on testing we know that fres uses only the first 15 bits of the mantissa
	// seee eeee eeee mmmm mmmm mmmm mmmx xxxx ....		(s = sign, e = exponent, m = mantissa, x = not used)
	// the mantissa bits are interpreted as following:
	// 0000 0000 0000 iiii ifff ffff fff0 ...			(i = table look up index , f = step multiplier)
	unsigned long long x = *(unsigned long long*)&input;

	// get index
	uint32 idx = (x >> (ieee_double_m_bits - espresso_fres_i_bits))&((1 << espresso_fres_i_bits) - 1);
	// get step multiplier
	uint32 stepMul = (x >> (ieee_double_m_bits - espresso_fres_i_bits - 10))&((1 << 10) - 1);


	uint32 sum = fresLookupTable[idx].offset - (fresLookupTable[idx].step * stepMul + 1) / 2;

	// get exponent
	uint32 e = (x >> ieee_double_m_bits) & ((1ull << ieee_double_e_bits) - 1ull);
	if (e == 0)
	{
		// todo?
		//x &= 0x7FFFFFFFFFFFFFFFull;
		x |= 0x7FF0000000000000ull;
		return *(double*)&x;
	}
	else if (e == 0x7ff) // NaN or INF
	{
		if ((x&((1ull << ieee_double_m_bits) - 1)) == 0)
		{
			// negative INF returns -0.0
			if ((sint64)x < 0)
			{
				x = 0x8000000000000000;
				return *(double*)&x;
			}
			// positive INF returns +0.0
			return 0.0;
		}
		// result is NaN with same sign and same mantissa (todo: verify)
		return *(double*)&x;
	}
	// todo - needs more testing (especially NaN and INF values)

	e = 2045 - e;
	x &= ~(((1ull << ieee_double_e_bits) - 1ull) << ieee_double_m_bits);
	x |= ((unsigned long long)e << ieee_double_m_bits);

	x &= ~((1ull << ieee_double_m_bits) - 1ull);
	x += ((unsigned long long)sum << 29ull);

	return *(double*)&x;
}

void fcmpu_espresso(PPCInterpreter_t* hCPU, int crfD, double a, double b)
{
	uint32 c;

	ppc_setCRBit(hCPU, crfD + 0, 0);
	ppc_setCRBit(hCPU, crfD + 1, 0);
	ppc_setCRBit(hCPU, crfD + 2, 0);
	ppc_setCRBit(hCPU, crfD + 3, 0);

	if (IS_NAN(*(uint64*)&a) || IS_NAN(*(uint64*)&b))
	{
		c = 1;
		ppc_setCRBit(hCPU, crfD + CR_BIT_SO, 1);
	}
	else if (a < b)
	{
		c = 8;
		ppc_setCRBit(hCPU, crfD + CR_BIT_LT, 1);
	}
	else if (a > b)
	{
		c = 4;
		ppc_setCRBit(hCPU, crfD + CR_BIT_GT, 1);
	}
	else
	{
		c = 2;
		ppc_setCRBit(hCPU, crfD + CR_BIT_EQ, 1);
	}

	if (IS_SNAN(*(uint64*)&a) || IS_SNAN(*(uint64*)&b))
		hCPU->fpscr |= FPSCR_VXSNAN;

	hCPU->fpscr = (hCPU->fpscr & 0xffff0fff) | (c << 12);
}

void PPCInterpreter_FMR(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, rA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, frB);
	PPC_ASSERT(rA==0);
	hCPU->fpr[frD].fpr = hCPU->fpr[frB].fpr;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FSEL(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	if ( hCPU->fpr[frA].fp0 >= -0.0f )
		hCPU->fpr[frD] = hCPU->fpr[frC];
	else
		hCPU->fpr[frD] = hCPU->fpr[frB];
	PPC_ASSERT((Opcode & PPC_OPC_RC) != 0); // update CR1 flags

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FCTIWZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	double b = hCPU->fpr[frB].fpr;
	uint64 v;
	if (b > (double)0x7FFFFFFF)
	{
		v = (uint64)0x7FFFFFFF;
	}
	else if (b < -(double)0x80000000)
	{
		v = (uint64)0x80000000;
	}
	else
	{
		v = (uint64)(uint32)(sint32)b;
	}

	hCPU->fpr[frD].guint = 0xFFF8000000000000ULL | v;
	if (v == 0 && ((*(uint64*)&b) >> 63))
		hCPU->fpr[frD].guint |= 0x100000000ull;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FCTIW(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	double b = hCPU->fpr[frB].fpr;
	uint64 v;
	if (b > (double)0x7FFFFFFF)
	{
		v = (uint64)0x7FFFFFFF;
	}
	else if (b < -(double)0x80000000)
	{
		v = (uint64)0x80000000;
	}
	else
	{
		// Honour FPSCR[RN] via the host mode set by PPCInterpreter_setRoundingModeFromFPSCR.
		// lrint uses the current C rounding mode; the old +0.5 path only ever did round-half-up.
		v = (uint64)(uint32)(sint32)std::lrint(b);
	}
	hCPU->fpr[frD].guint = 0xFFF8000000000000ULL | v;
	if (v == 0 && ((*(uint64*)&b) >> 63))
		hCPU->fpr[frD].guint |= 0x100000000ull;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNEG(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	
	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint ^ (1ULL << 63);

	PPC_ASSERT((Opcode & PPC_OPC_RC) != 0); // update CR1 flags

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FRSP(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	if( PPC_PSE )
	{
		hCPU->fpr[frD].fp0 = (float)hCPU->fpr[frB].fpr;
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;
	}
	else
	{
		hCPU->fpr[frD].fpr = (float)hCPU->fpr[frB].fpr;
	}

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FRSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frA==0 && frC==0);
	
	hCPU->fpr[frD].fpr = frsqrte_espresso(hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FRES(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frA==0 && frC==0);

	hCPU->fpr[frD].fpr = fres_espresso(hCPU->fpr[frB].fpr);
	
	if(PPC_PSE) 
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

// Floating point ALU

void PPCInterpreter_FABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint & ~0x8000000000000000;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	
	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint | 0x8000000000000000;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	hCPU->fpr[frD].fpr = hCPU->fpr[frA].fpr + hCPU->fpr[frB].fpr;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FDIV(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	hCPU->fpr[frD].fpr = hCPU->fpr[frA].fpr / hCPU->fpr[frB].fpr;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	hCPU->fpr[frD].fpr = hCPU->fpr[frA].fpr - hCPU->fpr[frB].fpr;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMUL(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC == 0);

	hCPU->fpr[frD].fpr = hCPU->fpr[frA].fpr * hCPU->fpr[frC].fpr;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fmadd(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNMADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fnmadd(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fmsub(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNMSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fnmsub(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

// Move

void PPCInterpreter_MFFS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, rA, rB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, rB);
	PPC_ASSERT(rA==0 && rB==0);
	hCPU->fpr[frD].guint = (uint64)hCPU->fpscr;

	PPCInterpreter_nextInstruction(hCPU);
}


void PPCInterpreter_setRoundingModeFromFPSCR(PPCInterpreter_t* hCPU)
{
	// Per host thread, not per guest thread: guest threads are fibers sharing a host thread, so the
	// host FPU mode is shared between them. The context-load path re-syncs on every switch.
	static thread_local uint32 s_appliedRN = 0xFFFFFFFFu;
	const uint32 rn = hCPU->fpscr & 3;
	if (rn == s_appliedRN) [[likely]]
		return;
	s_appliedRN = rn;
	// PowerPC RN encoding, PEM 2.1.4: 00 nearest, 01 toward zero, 10 toward +inf, 11 toward -inf.
	static constexpr int kHostMode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
	fesetround(kHostMode[rn]);
}


// mtfsfi crfD,IMM -- set one 4-bit FPSCR field to an immediate. It was not implemented at all:
// the opcode-63 dispatcher had no case 134, so it fell through to cemu_assert_unimplemented and
// the instruction did nothing. That matters more than the usual unimplemented-instruction gap,
// because `mtfsfi 7,n` is the standard way a guest selects a rounding mode, and field 7 is the
// one holding FPSCR[RN]. Caught by testing/rom-tests' fp_rounding_mode_honoured.
//
// Field crfD spans FPSCR bits 4*crfD .. 4*crfD+3 in PowerPC's MSB-0 numbering, which is a shift
// of 28 - 4*crfD in a plain uint32.
void PPCInterpreter_MTFSFI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	const uint32 crfD = (Opcode >> 23) & 7;
	const uint32 imm = (Opcode >> 12) & 0xF;
	const uint32 shift = 28u - 4u * crfD;
	hCPU->fpscr = (hCPU->fpscr & ~(0xFu << shift)) | (imm << shift);
	PPCInterpreter_setRoundingModeFromFPSCR(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_MTFSF(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frB;
	uint32 fm, FM;
	PPC_OPC_TEMPL_XFL(Opcode, frB, fm);
	FM = ((fm&0x80)?0xf0000000:0)|((fm&0x40)?0x0f000000:0)|((fm&0x20)?0x00f00000:0)|((fm&0x10)?0x000f0000:0)|
	     ((fm&0x08)?0x0000f000:0)|((fm&0x04)?0x00000f00:0)|((fm&0x02)?0x000000f0:0)|((fm&0x01)?0x0000000f:0);
	hCPU->fpscr = (hCPU->fpr[frB].guint & FM) | (hCPU->fpscr & ~FM);
	PPCInterpreter_setRoundingModeFromFPSCR(hCPU);

	PPC_ASSERT((Opcode & PPC_OPC_RC) != 0); // update CR1 flags

	static bool logFPSCRWriteOnce = false;
	if( logFPSCRWriteOnce == false )
	{
		cemuLog_log(LogType::Force, "Unsupported write to FPSCR");
		logFPSCRWriteOnce = true;
	}
	PPCInterpreter_nextInstruction(hCPU);
}

// single precision

void PPCInterpreter_FADDS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frB == 0);
	
	// todo: check for RC

	hCPU->fpr[frD].fpr = (float)(hCPU->fpr[frA].fpr + hCPU->fpr[frB].fpr);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FSUBS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frB == 0);

	hCPU->fpr[frD].fpr = (float)(hCPU->fpr[frA].fpr - hCPU->fpr[frB].fpr);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FDIVS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frB==0);

	hCPU->fpr[frD].fpr = (float)(hCPU->fpr[frA].fpr / hCPU->fpr[frB].fpr);
	if( PPC_PSE )
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMULS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frB == 0);

	hCPU->fpr[frD].fpr = (float)(hCPU->fpr[frA].fpr * roundTo25BitAccuracy(hCPU->fpr[frC].fpr));
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

// Single-precision *S variants: round numeric results to float, but leave
// selected/generated NaNs as the double-form quieted payload (suite compares
// bit-exact against lfd constants like f12).
static inline double ppc_maybe_round_single(double r)
{
	const uint64 bits = *(uint64*)&r;
	if (ppc_bits_is_nan(bits) || ppc_bits_is_inf(bits))
		return r;
	return (double)(float)r;
}

void PPCInterpreter_FMADDS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	// 25-bit frC is applied inside ppc_fmadds* (tracks Inf-from-HUGE).
	hCPU->fpr[frD].fpr = ppc_fmadds(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNMADDS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fnmadds(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr, hCPU->fpr[frB].fpr);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMSUBS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fp0);
	hCPU->fpr[frD].fp0 = ppc_fmsubs(hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNMSUBS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);

	ppc_fma_bind_dest(hCPU->fpr[frD].fp0);
	hCPU->fpr[frD].fp0 = ppc_fnmsubs(hCPU->fpr[frA].fp0, hCPU->fpr[frC].fp0, hCPU->fpr[frB].fp0);
	if (PPC_PSE)
		hCPU->fpr[frD].fp1 = hCPU->fpr[frD].fp0;

	PPCInterpreter_nextInstruction(hCPU);
}

// Compare

void PPCInterpreter_FCMPO(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	int crfD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, crfD, frA, frB);
	crfD >>= 2;
	hCPU->cr[crfD*4+0] = 0;
	hCPU->cr[crfD*4+1] = 0;
	hCPU->cr[crfD*4+2] = 0;
	hCPU->cr[crfD*4+3] = 0;

	uint32 c;
	if(IS_NAN(hCPU->fpr[frA].guint) || IS_NAN(hCPU->fpr[frB].guint))
	{
		c = 1;
		hCPU->cr[crfD*4+CR_BIT_SO] = 1;
	}
    else if(hCPU->fpr[frA].fpr < hCPU->fpr[frB].fpr)
	{
		c = 8;
		hCPU->cr[crfD*4+CR_BIT_LT] = 1;
	}
	else if(hCPU->fpr[frA].fpr > hCPU->fpr[frB].fpr)
	{
		c = 4;
		hCPU->cr[crfD*4+CR_BIT_GT] = 1;
	}
	else
	{
		c = 2;
		hCPU->cr[crfD*4+CR_BIT_EQ] = 1;
	}

    hCPU->fpscr = (hCPU->fpscr & 0xffff0fff) | (c << 12);

	if (IS_SNAN (hCPU->fpr[frA].guint) || IS_SNAN (hCPU->fpr[frB].guint))
		hCPU->fpscr |= FPSCR_VXSNAN;
	else if (!(hCPU->fpscr & FPSCR_VE) || IS_QNAN (hCPU->fpr[frA].guint) || IS_QNAN (hCPU->fpr[frB].guint))
		hCPU->fpscr |= FPSCR_VXVC;

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FCMPU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	int crfD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, crfD, frA, frB);
	cemu_assert_debug((crfD % 4) == 0);
	fcmpu_espresso(hCPU, crfD, hCPU->fpr[frA].fp0, hCPU->fpr[frB].fp0);

	PPCInterpreter_nextInstruction(hCPU);
}

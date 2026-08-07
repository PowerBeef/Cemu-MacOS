#include <cfenv>
#include <cfloat>
#include <arm_acle.h>
#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

#include <cmath>
#include <math.h>
#include <cstring>
#include <atomic>

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
	// Bit-copy: host FZ must not flush denorm inputs before we see them.
	unsigned long long x;
	std::memcpy(&x, &input, sizeof(x));

	// 0.0 and -0.0
	if ((x << 1) == 0)
	{
		// result is inf or -inf (same sign)
		x &= ~0x7FFFFFFFFFFFFFFFULL;
		x |= 0x7FF0000000000000ULL;
		double out;
		std::memcpy(&out, &x, sizeof(out));
		return out;
	}
	// get exponent
	uint32 e = (x >> ieee_double_m_bits) & ((1ull << ieee_double_e_bits) - 1ull);
	// NaN or INF
	if (e == 0x7FF)
	{
		if ((x & ((1ull << ieee_double_m_bits) - 1)) == 0)
		{
			// negative INF returns +NaN
			if ((sint64)x < 0)
			{
				x = 0x7FF8000000000000ULL;
				double out;
				std::memcpy(&out, &x, sizeof(out));
				return out;
			}
			// positive INF returns +0.0
			return 0.0;
		}
		// Quiet SNaN; keep QNaN payload (suite: frsqrte SNaN → quieted).
		x = x | 0x0008000000000000ULL;
		double out;
		std::memcpy(&out, &x, sizeof(out));
		return out;
	}
	// negative number (other than -0.0)
	if ((sint64)x < 0)
	{
		// result is positive NaN
		x = 0x7FF8000000000000ULL;
		double out;
		std::memcpy(&out, &x, sizeof(out));
		return out;
	}

	// Denormals: normalize mantissa and adjust exponent (Espresso / Dolphin).
	// Suite: 1/sqrt(min double denorm) → 0x617FFE80_00000000.
	uint64 mant = x & ((1ull << ieee_double_m_bits) - 1ull);
	sint32 e_signed = (sint32)e;
	if (e == 0)
	{
		// value = 2^(1-1023) * (mant/2^52); shift until hidden bit appears
		e_signed = 1;
		if (mant == 0)
		{
			// +0 handled above; should not reach here
			x = 0x7FF0000000000000ULL;
			double out;
			std::memcpy(&out, &x, sizeof(out));
			return out;
		}
		while ((mant & (1ull << ieee_double_m_bits)) == 0)
		{
			mant <<= 1;
			e_signed--;
		}
		mant &= (1ull << ieee_double_m_bits) - 1ull;
	}

	// Rebuild bits for table index (exp field may be out of 11-bit range for denorms).
	const uint64 x_for_idx =
		((uint64)((uint32)e_signed & 0x7FFu) << ieee_double_m_bits) | mant;
	uint32 idx = (x_for_idx >> (ieee_double_m_bits - espresso_frsqrte_i_bits + 1ull)) &
		((1u << espresso_frsqrte_i_bits) - 1);
	uint32 stepMul = (x_for_idx >> (ieee_double_m_bits - espresso_frsqrte_i_bits + 1 - 11)) &
		((1u << 11) - 1);

	sint32 sum = frsqrteLookupTable[idx].offset - frsqrteLookupTable[idx].step * stepMul;

	// Signed exponent math so denorm e_signed < 0 still works.
	const sint32 e_out = 1023 - ((e_signed - 1021) >> 1);
	x = ((uint64)((uint32)e_out & 0x7FFu) << ieee_double_m_bits) |
		(((uint64)(uint32)sum) << 26ull);

	double out;
	std::memcpy(&out, &x, sizeof(out));
	return out;
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

// Core 25-bit round. Host FZ must already be clear — restoring FZ before a
// denormal result is consumed flushes it to zero (breaks ∞·min_denorm → ∞).
static inline double roundTo25BitAccuracy_nofz(double d)
{
	const uint64 v = *(const uint64*)&d;
	if ((v & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
		return d;
	if ((v & 0x7FFFFFFFFFFFFFFFULL) == 0)
		return d; // ±0

	int exp = 0;
	double m = std::frexp(std::fabs(d), &exp); // [0.5, 1)
	uint64 mb = *(uint64*)&m;
	const uint64 rounded = (mb & 0xFFFFFFFFF8000000ULL) + (mb & 0x8000000ULL);
	if ((rounded & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
		return std::copysign(std::numeric_limits<double>::infinity(), d);
	double rm = *(double*)&rounded;
	if (rm >= 1.0)
	{
		rm = 0.5;
		exp += 1;
	}
	// ldexp may overflow to Inf (DBL_MAX → 2^1024).
	return std::copysign(std::ldexp(rm, exp), d);
}

ATTR_MS_ABI double roundTo25BitAccuracy(double d)
{
	// Espresso single-precision product factor for frC: 25-bit significand,
	// round-half-up on the next bit. Denormals are normalized before rounding
	// (suite: 2^1023 * denorm). Round-up of DBL_MAX overflows to Inf.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	const double r = roundTo25BitAccuracy_nofz(d);
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	return r;
}

// Like roundTo25BitAccuracy_nofz, but reports whether a finite input overflowed to Inf.
// Host FZ must already be clear (see roundTo25BitAccuracy_nofz).
static inline double roundTo25BitAccuracyEx(double d, bool* outOverflowToInf)
{
	if (outOverflowToInf)
		*outOverflowToInf = false;
	const uint64 vIn = *(const uint64*)&d;
	const bool wasFinite = ((vIn & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL);
	const double r = roundTo25BitAccuracy_nofz(d);
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

// Prior frD + VE/ZE, set by ppc_fma_bind_dest before each helper call so both
// the interpreter and the recompiler can honour FPSCR result suppression.
static thread_local double s_fma_prev = 0.0;
static thread_local bool s_fma_ve = false;
static thread_local bool s_fma_ze = false;
static thread_local bool s_fma_suppressed = false;

ATTR_MS_ABI void ppc_fma_bind_dest(double prevFrD)
{
	s_fma_prev = prevFrD;
	s_fma_suppressed = false;
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	s_fma_ve = hCPU && (hCPU->fpscr & FPSCR_VE);
	s_fma_ze = hCPU && (hCPU->fpscr & FPSCR_ZE);
}

ATTR_MS_ABI bool ppc_fma_was_suppressed()
{
	return s_fma_suppressed;
}

static inline double ppc_fp_finish_ze(double specialResult)
{
	// ZE + exact zero divisor → leave destination unchanged (suite check_fpu_noresult).
	if (s_fma_ze)
	{
		s_fma_suppressed = true;
		return s_fma_prev;
	}
	s_fma_suppressed = false;
	return specialResult;
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
	// Keep FZ clear for the whole path so denormal frC/frA survive 25-bit + product.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));

	bool cOverflowToInf = false;
	double c = roundTo25BitAccuracyEx(cIn, &cOverflowToInf);

	double special;
	const int kind = ppc_fmadd_try_special(a, b, c, isMsub, &special, cOverflowToInf);
	if (kind != 0)
	{
		if (fpcr & (1ull << 24))
			__arm_wsr64("fpcr", fpcr);
		return ppc_fma_finish_special(kind, special);
	}
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
			// FZ already clear for this whole function.
			const uint32 sr = ppc_f32_to_bits(std::fmaf(fa, fc, fb));
			const uint64 dr = ConvertToDoubleNoFTZ(sr);
			r = *(double*)&dr;
		}
	}
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
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

// Pack multiply result to single-domain FPR: IEEE RN via (float), not
// ConvertToSingleNoFTZ truncate (suite: 1.5·1.333…25 → 2.0, not 0x3FFFFFFF).
static inline double ppc_mul_result_to_single(double r)
{
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	const float f = static_cast<float>(r);
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	const uint32 sr = ppc_f32_to_bits(f);
	const uint64 dr = ConvertToDoubleNoFTZ(sr);
	return *(double*)&dr;
}

// Multiply specials (NaN A→C; 0·∞ / ∞·0 → default QNaN).
// cOverflowToInf: 25-bit of finite HUGE — not a true Inf for VXIMZ (0·HUGE → 0).
// singleDomain: quieted NaN is folded through float bits (fmuls); double keeps payload.
static int ppc_fmul_try_special(double a, double c, double* out, bool cOverflowToInf, bool singleDomain)
{
	const uint64 ua = *(const uint64*)&a;
	const uint64 uc = *(const uint64*)&c;

	const bool snanA = ppc_bits_is_snan(ua);
	const bool snanC = ppc_bits_is_snan(uc);
	const bool anySNaN = snanA || snanC;
	const bool nanA = ppc_bits_is_nan(ua);
	const bool nanC = ppc_bits_is_nan(uc);

	if (nanA || nanC)
	{
		const uint64 selected = ppc_quiet_nan(nanA ? ua : uc);
		if (singleDomain)
		{
			// Suite: 1·SNaN(-1) → 0xFFFFFFFFE0000000 after single fold.
			const uint32 sr = ConvertToSingleNoFTZ(selected);
			const uint64 dr = ConvertToDoubleNoFTZ(sr);
			*out = *(double*)&dr;
		}
		else
			*out = *(double*)&selected;
		return anySNaN ? 2 : 1;
	}

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
	return 0;
}

// Single-precision multiply: raw frC, 25-bit + ldexp(|a|,1024) when C overflows.
// Suite: min_denorm·HUGE → finite; 0·HUGE → 0; HUGE·HUGE → +Inf; ∞·min_denorm → ∞.
ATTR_MS_ABI double ppc_fmuls(double a, double cIn)
{
	// FZ clear for whole function — denormal frC must not flush between 25-bit and mul.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));

	bool cOverflowToInf = false;
	double c = roundTo25BitAccuracyEx(cIn, &cOverflowToInf);

	double special;
	const int kind = ppc_fmul_try_special(a, c, &special, cOverflowToInf, true);
	if (kind != 0)
	{
		if (fpcr & (1ull << 24))
			__arm_wsr64("fpcr", fpcr);
		return ppc_fma_finish_special(kind, special);
	}
	s_fma_suppressed = false;

	double prod;
	if (cOverflowToInf)
	{
		const double mag = std::ldexp(std::fabs(a), 1024);
		prod = (std::signbit(a) != std::signbit(c)) ? -mag : mag;
	}
	else
		prod = a * c;

	const double result = ppc_mul_result_to_single(prod);
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	return result;
}

// Espresso fres table from ppc750cl.s calc_fres (base,delta) pairs — 32 intervals.
static const uint16 s_fresBase[32] = {
	0x3FFC,0x3C1C,0x3875,0x3504,0x31C4,0x2EB1,0x2BC8,0x2904,
	0x2664,0x23E5,0x2184,0x1F40,0x1D16,0x1B04,0x190A,0x1725,
	0x1554,0x1396,0x11EB,0x104F,0x0EC4,0x0D48,0x0BD7,0x0A7C,
	0x0922,0x07DF,0x069C,0x056F,0x0442,0x0328,0x020E,0x0106
};
static const uint16 s_fresDelta[32] = {
	0x3E1,0x3A7,0x371,0x340,0x313,0x2EA,0x2C4,0x2A0,
	0x27F,0x261,0x245,0x22A,0x212,0x1FB,0x1E5,0x1D1,
	0x1BE,0x1AC,0x19B,0x18B,0x17C,0x16E,0x15B,0x15B,
	0x143,0x143,0x12D,0x12D,0x11A,0x11A,0x108,0x106
};

// calc_fres normal path: table lookup + optional result denormalization.
static inline uint32 fres_normal_body(uint32 sign, sint32 expField, uint32 mant)
{
	sint32 r9 = 253 - expField;
	const uint32 idx = (mant >> 18) & 0x1Fu;
	const uint32 stepMul = (mant >> 8) & 0x3FFu;
	uint32 m = ((uint32)s_fresBase[idx] << 10) - (uint32)s_fresDelta[idx] * stepMul;
	m >>= 1;
	if (r9 <= 0)
	{
		const bool wasZero = (r9 == 0);
		r9 = 0;
		m |= 0x00800000u;
		m >>= 1;
		if (!wasZero)
			m >>= 1;
	}
	return sign | ((uint32)r9 << 23) | (m & 0x007FFFFFu);
}

ATTR_MS_ABI double fres_espresso(double input)
{
	// fres is a single-precision estimate: operate on float bits (suite calc_fres
	// does stfs→lwz), then expand the 32-bit result back to the FPR.
	const uint32 fb = ConvertToSingleNoFTZ(*(const uint64*)&input);
	const uint32 sign = fb & 0x80000000u;
	uint32 exp = (fb >> 23) & 0xFFu;
	uint32 mant = fb & 0x007FFFFFu;

	uint32 out;
	if (exp == 255)
	{
		if (mant == 0)
			out = sign; // ±Inf → ±0
		else
			out = fb | 0x00400000u; // quiet NaN
	}
	else if (exp == 0)
	{
		if (mant == 0)
			out = sign | 0x7F800000u; // ±0 → ±Inf
		else if (mant < 0x200000u)
			out = sign | 0x7F7FFFFFu; // tiny denorm → ±HUGE_VALF
		else
		{
			// Normalize denormal (1 or 2 shifts), then normal body with expField 0 or -1.
			sint32 expField = 0;
			mant <<= 1;
			if ((mant & 0x00800000u) == 0)
			{
				mant <<= 1;
				expField = -1;
			}
			mant &= 0x007FFFFFu;
			out = fres_normal_body(sign, expField, mant);
		}
	}
	else
		out = fres_normal_body(sign, (sint32)exp, mant);

	const uint64 dr = ConvertToDoubleNoFTZ(out);
	return *(double*)&dr;
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
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

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

// fctiw[z]: convert to 32-bit signed int, pack as 0xFFF8000x_xxxxxxxx (750CL high word).
// NaN → 0x80000000 (VXCVI; +VXSNAN for SNaN). Out-of-range / Inf → clamp + VXCVI.
// VE suppresses the write (suite check_fpu_noresult_nofprf).
static inline double ppc_fctiw_pack(uint32 v, bool negZero)
{
	uint64 r = 0xFFF8000000000000ULL | (uint64)v;
	if (negZero)
		r |= 0x100000000ull; // high word 0xFFF80001 for −0 → 0
	return *(double*)&r;
}

static inline double ppc_fctiw_common(double b, bool roundTowardZero)
{
	const uint64 ub = *(const uint64*)&b;
	const bool isNan = ppc_bits_is_nan(ub);
	const bool isSNan = ppc_bits_is_snan(ub);
	const bool isInf = ppc_bits_is_inf(ub);
	const bool sign = (ub >> 63) != 0;

	uint32 v;
	bool invalid = false;

	if (isNan)
	{
		v = 0x80000000u;
		invalid = true;
	}
	else if (b > (double)0x7FFFFFFF || (isInf && !sign))
	{
		v = 0x7FFFFFFFu;
		invalid = true;
	}
	else if (b < -(double)0x80000000 || (isInf && sign))
	{
		v = 0x80000000u;
		invalid = true;
	}
	else
	{
		if (roundTowardZero)
			v = (uint32)(sint32)b; // trunc toward zero
		else
			v = (uint32)(sint32)std::lrint(b); // FPSCR[RN] via host mode
		// Exact 2^31 is already handled by the > 0x7FFFFFFF test; in-range only here.
	}

	// kind 2 = invalid for VE suppress (SNaN or VXCVI).
	if (invalid || isSNan)
	{
		const double packed = ppc_fctiw_pack(v, false);
		return ppc_fma_finish_special(2, packed);
	}

	s_fma_suppressed = false;
	// −0 → integer 0 with high-word low bit set (suite fctiw high-word tests).
	const bool negZero = (v == 0) && sign;
	return ppc_fctiw_pack(v, negZero);
}

ATTR_MS_ABI double ppc_fctiw(double b)
{
	return ppc_fctiw_common(b, false);
}

ATTR_MS_ABI double ppc_fctiwz(double b)
{
	return ppc_fctiw_common(b, true);
}

void PPCInterpreter_FCTIWZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fctiwz(hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FCTIW(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fctiw(hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNEG(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	
	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint ^ (1ULL << 63);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

// frsp: round double → single → re-expand. Host FZ is set for Espresso (coreinit
// thread entry); it must stay clear for BOTH the down-cast and the re-expand —
// restoring FZ before (double)s flushes a denormal float input to 0 (suite: min
// single denorm exact). SNaN quiets; VE suppresses the write.
ATTR_MS_ABI double ppc_frsp(double b)
{
	const uint64 ub = *(const uint64*)&b;
	if (ppc_bits_is_nan(ub))
	{
		if (ppc_bits_is_snan(ub))
		{
			const uint64 q = ppc_quiet_nan(ub);
			return ppc_fma_finish_special(2, *(double*)&q);
		}
		// QNaN: preserve exact payload (suite fmr %f4,%f10).
		s_fma_suppressed = false;
		return b;
	}

	// Host RN already tracks FPSCR via PPCInterpreter_setRoundingModeFromFPSCR.
	// Keep FZ clear across both casts; volatile blocks reordering across the MSR.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	volatile double vb = b;
	volatile float s = (float)vb;
	volatile double r = (double)s;
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);

	s_fma_suppressed = false;
	return r;
}

void PPCInterpreter_FRSP(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	
	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	const double r = ppc_frsp(hCPU->fpr[frB].fpr);
	if (PPC_PSE)
	{
		hCPU->fpr[frD].fp0 = r;
		hCPU->fpr[frD].fp1 = r;
	}
	else
	{
		hCPU->fpr[frD].fpr = r;
	}

	PPCInterpreter_nextInstruction(hCPU);
}

// Unary estimate with VE (SNaN) / ZE (±0 for fres) suppress.
ATTR_MS_ABI double ppc_fres(double b)
{
	const uint64 ub = *(const uint64*)&b;
	if (ppc_bits_is_snan(ub))
	{
		double q = fres_espresso(b); // quiets
		return ppc_fma_finish_special(2, q);
	}
	if (ppc_bits_is_zero(ub))
	{
		// ±0 → ±Inf; ZE suppresses write.
		double r = fres_espresso(b);
		return ppc_fp_finish_ze(r);
	}
	s_fma_suppressed = false;
	return fres_espresso(b);
}

ATTR_MS_ABI double ppc_frsqrte(double b)
{
	const uint64 ub = *(const uint64*)&b;
	if (ppc_bits_is_snan(ub) || (ppc_bits_is_inf(ub) && (ub >> 63)) ||
		(!ppc_bits_is_zero(ub) && !ppc_bits_is_nan(ub) && !ppc_bits_is_inf(ub) && (ub >> 63)))
	{
		// SNaN, −Inf, or negative finite → invalid (NaN); VE suppresses.
		// Negative zero is allowed (→ −Inf).
		if (ppc_bits_is_snan(ub) || ((ub >> 63) && !ppc_bits_is_zero(ub)))
		{
			double r = frsqrte_espresso(b);
			return ppc_fma_finish_special(2, r);
		}
	}
	if (ppc_bits_is_zero(ub))
	{
		double r = frsqrte_espresso(b); // ±0 → ±Inf
		return ppc_fp_finish_ze(r);
	}
	s_fma_suppressed = false;
	return frsqrte_espresso(b);
}

// Double-precision mul: NaN A→C, 0·∞ → default QNaN, VE suppress.
ATTR_MS_ABI double ppc_fmul(double a, double c)
{
	double special;
	const int kind = ppc_fmul_try_special(a, c, &special, false, false);
	if (kind != 0)
		return ppc_fma_finish_special(kind, special);
	s_fma_suppressed = false;
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	const double r = a * c;
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	return r;
}

// Double-precision div: NaN A→B, 0/0 & ∞/∞ → default QNaN, x/0 with ZE suppress.
ATTR_MS_ABI double ppc_fdiv(double a, double b)
{
	const uint64 ua = *(const uint64*)&a;
	const uint64 ub = *(const uint64*)&b;
	const bool snanA = ppc_bits_is_snan(ua);
	const bool snanB = ppc_bits_is_snan(ub);
	const bool nanA = ppc_bits_is_nan(ua);
	const bool nanB = ppc_bits_is_nan(ub);

	if (nanA || nanB)
	{
		const uint64 selected = nanA ? ua : ub;
		const uint64 q = ppc_quiet_nan(selected);
		double out = *(double*)&q;
		return ppc_fma_finish_special((snanA || snanB) ? 2 : 1, out);
	}

	const bool zA = ppc_bits_is_zero(ua);
	const bool zB = ppc_bits_is_zero(ub);
	const bool iA = ppc_bits_is_inf(ua);
	const bool iB = ppc_bits_is_inf(ub);

	if (zA && zB)
	{
		// 0/0 → VXZDZ default QNaN
		double out = *(double*)&kPpcDefaultQNaN;
		return ppc_fma_finish_special(2, out);
	}
	if (iA && iB)
	{
		// ∞/∞ → VXIDI default QNaN
		double out = *(double*)&kPpcDefaultQNaN;
		return ppc_fma_finish_special(2, out);
	}
	if (zB && !zA)
	{
		// x/0 → ±Inf; ZE suppresses
		const uint64 sign = (ua ^ ub) & 0x8000000000000000ULL;
		const uint64 inf = sign | 0x7FF0000000000000ULL;
		double out = *(double*)&inf;
		return ppc_fp_finish_ze(out);
	}

	// FZ clear so tininess (UX underflows that round up to min normal) is not
	// flushed to zero by host FPCR.FZ (suite: (2·min_normal−ulp)/2).
	// volatile + barriers: clang otherwise restores FPCR before the fdiv.
	s_fma_suppressed = false;
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	std::atomic_signal_fence(std::memory_order_seq_cst);
	volatile double va = a, vb = b;
	volatile double r = va / vb;
	std::atomic_signal_fence(std::memory_order_seq_cst);
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	return r;
}

void PPCInterpreter_FRSQRTE(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frA==0 && frC==0);
	
	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_frsqrte(hCPU->fpr[frB].fpr);
	ppc_fpscr_set_fprf_from_double(hCPU->fpscr, hCPU->fpr[frD].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FRES(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frA==0 && frC==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fres(hCPU->fpr[frB].fpr);
	
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

	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint & ~0x8000000000000000ULL;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FNABS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB;
	PPC_OPC_TEMPL_X(Opcode, frD, frA, frB);
	PPC_ASSERT(frA==0);
	
	hCPU->fpr[frD].guint = hCPU->fpr[frB].guint | 0x8000000000000000ULL;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

	PPCInterpreter_nextInstruction(hCPU);
}

// Add/sub specials: NaN A→B; ∞−∞ / ∞+−∞ → default QNaN; VE suppress.
static int ppc_faddsub_try_special(double a, double b, bool isSub, double* out)
{
	const uint64 ua = *(const uint64*)&a;
	const uint64 ub = *(const uint64*)&b;
	const bool snanA = ppc_bits_is_snan(ua);
	const bool snanB = ppc_bits_is_snan(ub);
	const bool nanA = ppc_bits_is_nan(ua);
	const bool nanB = ppc_bits_is_nan(ub);
	if (nanA || nanB)
	{
		const uint64 q = ppc_quiet_nan(nanA ? ua : ub);
		*out = *(double*)&q;
		return (snanA || snanB) ? 2 : 1;
	}
	if (ppc_bits_is_inf(ua) && ppc_bits_is_inf(ub))
	{
		const uint64 sa = ua & 0x8000000000000000ULL;
		uint64 sb = ub & 0x8000000000000000ULL;
		if (isSub)
			sb ^= 0x8000000000000000ULL;
		if (sa != sb)
		{
			// opposite-signed infinities cancel → VXISI
			*out = *(double*)&kPpcDefaultQNaN;
			return 2;
		}
	}
	return 0;
}

ATTR_MS_ABI double ppc_fadd(double a, double b)
{
	double special;
	const int kind = ppc_faddsub_try_special(a, b, false, &special);
	if (kind != 0)
		return ppc_fma_finish_special(kind, special);
	s_fma_suppressed = false;
	return a + b;
}

ATTR_MS_ABI double ppc_fsub(double a, double b)
{
	double special;
	const int kind = ppc_faddsub_try_special(a, b, true, &special);
	if (kind != 0)
		return ppc_fma_finish_special(kind, special);
	s_fma_suppressed = false;
	return a - b;
}

// Fold double bits through single without IEEE float cast (preserves the
// freescale denorm/tiny encoding used by stfs and by merge of rsqrte results).
static inline double ppc_ps_bit_fold_single(uint64 b)
{
	const uint32 s = ConvertToSingleNoFTZ(b);
	const uint64 e = ConvertToDoubleNoFTZ(s);
	return *(const double*)&e;
}

// PS underflow sticky encoding: expand(min single normal) with low bit set.
// Distinct from a clean lfs of min normal (low bits 0). stfd stores 0; stfs
// stores min normal (suite denorm-merge dual view).
static constexpr uint64 kPsUnderflowStickyAbs = 0x3810000000000001ULL;

static inline double ppc_ps_underflow_sticky(uint64 signBit)
{
	const uint64 bits = (signBit & 0x8000000000000000ULL) | kPsUnderflowStickyAbs;
	double out;
	std::memcpy(&out, &bits, sizeof(out));
	return out;
}

static inline bool ppc_ps_is_underflow_sticky(uint64 b)
{
	return (b & 0x7FFFFFFFFFFFFFFFULL) == kPsUnderflowStickyAbs;
}

// PS lane quantize (RN) for merge slot0, mr/neg/abs, and finite add/sub results.
// Suite: "Slot 0 is rounded". Host float RN for values in the single-normal
// window and above; freescale bit fold for tinies (exp < 896).
// Underflow of nonzero → sticky min-normal encoding (stfd→0 / stfs→min normal).
ATTR_MS_ABI double ppc_ps_quantize(double d)
{
	uint64 b;
	std::memcpy(&b, &d, sizeof(b));
	if (ppc_ps_is_underflow_sticky(b))
		return d; // keep sticky through merge/mr
	const uint64 abs = b & 0x7FFFFFFFFFFFFFFFULL;
	if (abs == 0)
		return d;
	if (abs >= 0x7FF0000000000000ULL)
		return ppc_ps_bit_fold_single(b);
	const uint32 exp = (uint32)((b >> 52) & 0x7FFu);
	if (exp < 896)
	{
		const double folded = ppc_ps_bit_fold_single(b);
		uint64 fb;
		std::memcpy(&fb, &folded, sizeof(fb));
		if ((fb & 0x7FFFFFFFFFFFFFFFULL) == 0)
			return ppc_ps_underflow_sticky(b);
		return folded;
	}
	// Finite in single-normal/overflow range: host RN, FZ clear.
	const uint64 fpcr = __arm_rsr64("fpcr");
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
	volatile double vd = d;
	volatile float sf = (float)vd;
	volatile double r = (double)sf;
	if (fpcr & (1ull << 24))
		__arm_wsr64("fpcr", fpcr);
	uint64 rb;
	std::memcpy(&rb, (const void*)&r, sizeof(rb));
	if ((rb & 0x7FFFFFFFFFFFFFFFULL) == 0)
		return ppc_ps_underflow_sticky(b);
	return r;
}

// PS merge slot1: truncate to single via freescale bit path (no RN).
// Suite: "slot 1 is truncated"; HUGE → max normal single re-expanded.
ATTR_MS_ABI double ppc_ps_quantize_tz(double d)
{
	uint64 b;
	std::memcpy(&b, &d, sizeof(b));
	if (ppc_ps_is_underflow_sticky(b))
		return d;
	const uint64 abs = b & 0x7FFFFFFFFFFFFFFFULL;
	if (abs == 0)
		return d;
	const double folded = ppc_ps_bit_fold_single(b);
	uint64 fb;
	std::memcpy(&fb, &folded, sizeof(fb));
	if (abs < 0x7FF0000000000000ULL && (fb & 0x7FFFFFFFFFFFFFFFULL) == 0)
		return ppc_ps_underflow_sticky(b);
	return folded;
}

// lfd high-word → ps1 shadow (suite paired-single / double mode notes).
ATTR_MS_ABI double ppc_lfd_ps_shadow(double d)
{
	uint64 b;
	std::memcpy(&b, &d, sizeof(b));
	const uint64 s = ppc_lfd_shadow_from_double_bits(b);
	double out;
	std::memcpy(&out, &s, sizeof(out));
	return out;
}

ATTR_MS_ABI void ppc_isync_clear_ps_dirty()
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	if (hCPU)
		hCPU->psWriteDirty = 0;
}

ATTR_MS_ABI void ppc_note_ps_write(sint32 frD)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	if (hCPU && (uint32)frD < 32u)
		hCPU->psWriteDirty |= 1u << frD;
}

#define PPC_NOTE_PS_DEF(n) \
	ATTR_MS_ABI void ppc_note_ps_write_fr##n() { ppc_note_ps_write(n); }
PPC_NOTE_PS_DEF(0) PPC_NOTE_PS_DEF(1) PPC_NOTE_PS_DEF(2) PPC_NOTE_PS_DEF(3)
PPC_NOTE_PS_DEF(4) PPC_NOTE_PS_DEF(5) PPC_NOTE_PS_DEF(6) PPC_NOTE_PS_DEF(7)
PPC_NOTE_PS_DEF(8) PPC_NOTE_PS_DEF(9) PPC_NOTE_PS_DEF(10) PPC_NOTE_PS_DEF(11)
PPC_NOTE_PS_DEF(12) PPC_NOTE_PS_DEF(13) PPC_NOTE_PS_DEF(14) PPC_NOTE_PS_DEF(15)
PPC_NOTE_PS_DEF(16) PPC_NOTE_PS_DEF(17) PPC_NOTE_PS_DEF(18) PPC_NOTE_PS_DEF(19)
PPC_NOTE_PS_DEF(20) PPC_NOTE_PS_DEF(21) PPC_NOTE_PS_DEF(22) PPC_NOTE_PS_DEF(23)
PPC_NOTE_PS_DEF(24) PPC_NOTE_PS_DEF(25) PPC_NOTE_PS_DEF(26) PPC_NOTE_PS_DEF(27)
PPC_NOTE_PS_DEF(28) PPC_NOTE_PS_DEF(29) PPC_NOTE_PS_DEF(30) PPC_NOTE_PS_DEF(31)
#undef PPC_NOTE_PS_DEF

uintptr_t g_note_ps_write_fr_fn[32] = {
	(uintptr_t)ppc_note_ps_write_fr0,  (uintptr_t)ppc_note_ps_write_fr1,  (uintptr_t)ppc_note_ps_write_fr2,  (uintptr_t)ppc_note_ps_write_fr3,
	(uintptr_t)ppc_note_ps_write_fr4,  (uintptr_t)ppc_note_ps_write_fr5,  (uintptr_t)ppc_note_ps_write_fr6,  (uintptr_t)ppc_note_ps_write_fr7,
	(uintptr_t)ppc_note_ps_write_fr8,  (uintptr_t)ppc_note_ps_write_fr9,  (uintptr_t)ppc_note_ps_write_fr10, (uintptr_t)ppc_note_ps_write_fr11,
	(uintptr_t)ppc_note_ps_write_fr12, (uintptr_t)ppc_note_ps_write_fr13, (uintptr_t)ppc_note_ps_write_fr14, (uintptr_t)ppc_note_ps_write_fr15,
	(uintptr_t)ppc_note_ps_write_fr16, (uintptr_t)ppc_note_ps_write_fr17, (uintptr_t)ppc_note_ps_write_fr18, (uintptr_t)ppc_note_ps_write_fr19,
	(uintptr_t)ppc_note_ps_write_fr20, (uintptr_t)ppc_note_ps_write_fr21, (uintptr_t)ppc_note_ps_write_fr22, (uintptr_t)ppc_note_ps_write_fr23,
	(uintptr_t)ppc_note_ps_write_fr24, (uintptr_t)ppc_note_ps_write_fr25, (uintptr_t)ppc_note_ps_write_fr26, (uintptr_t)ppc_note_ps_write_fr27,
	(uintptr_t)ppc_note_ps_write_fr28, (uintptr_t)ppc_note_ps_write_fr29, (uintptr_t)ppc_note_ps_write_fr30, (uintptr_t)ppc_note_ps_write_fr31,
};

#define PPC_LFD_PS1_DEF(n) \
	ATTR_MS_ABI double ppc_lfd_ps1_fr##n(double loaded, double curPs1) \
	{ \
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance(); \
		if (!hCPU) return curPs1; \
		const uint32 bit = 1u << (n); \
		if ((hCPU->psWriteDirty & bit) == 0) return curPs1; \
		hCPU->psWriteDirty &= ~bit; \
		uint64 ps1bits; \
		std::memcpy(&ps1bits, &curPs1, sizeof(ps1bits)); \
		if ((ps1bits & 0x7FFFFFFFFFFFFFFFULL) != 0) return curPs1; \
		return ppc_lfd_ps_shadow(loaded); \
	}
PPC_LFD_PS1_DEF(0) PPC_LFD_PS1_DEF(1) PPC_LFD_PS1_DEF(2) PPC_LFD_PS1_DEF(3)
PPC_LFD_PS1_DEF(4) PPC_LFD_PS1_DEF(5) PPC_LFD_PS1_DEF(6) PPC_LFD_PS1_DEF(7)
PPC_LFD_PS1_DEF(8) PPC_LFD_PS1_DEF(9) PPC_LFD_PS1_DEF(10) PPC_LFD_PS1_DEF(11)
PPC_LFD_PS1_DEF(12) PPC_LFD_PS1_DEF(13) PPC_LFD_PS1_DEF(14) PPC_LFD_PS1_DEF(15)
PPC_LFD_PS1_DEF(16) PPC_LFD_PS1_DEF(17) PPC_LFD_PS1_DEF(18) PPC_LFD_PS1_DEF(19)
PPC_LFD_PS1_DEF(20) PPC_LFD_PS1_DEF(21) PPC_LFD_PS1_DEF(22) PPC_LFD_PS1_DEF(23)
PPC_LFD_PS1_DEF(24) PPC_LFD_PS1_DEF(25) PPC_LFD_PS1_DEF(26) PPC_LFD_PS1_DEF(27)
PPC_LFD_PS1_DEF(28) PPC_LFD_PS1_DEF(29) PPC_LFD_PS1_DEF(30) PPC_LFD_PS1_DEF(31)
#undef PPC_LFD_PS1_DEF

uintptr_t g_lfd_ps1_fr_fn[32] = {
	(uintptr_t)ppc_lfd_ps1_fr0,  (uintptr_t)ppc_lfd_ps1_fr1,  (uintptr_t)ppc_lfd_ps1_fr2,  (uintptr_t)ppc_lfd_ps1_fr3,
	(uintptr_t)ppc_lfd_ps1_fr4,  (uintptr_t)ppc_lfd_ps1_fr5,  (uintptr_t)ppc_lfd_ps1_fr6,  (uintptr_t)ppc_lfd_ps1_fr7,
	(uintptr_t)ppc_lfd_ps1_fr8,  (uintptr_t)ppc_lfd_ps1_fr9,  (uintptr_t)ppc_lfd_ps1_fr10, (uintptr_t)ppc_lfd_ps1_fr11,
	(uintptr_t)ppc_lfd_ps1_fr12, (uintptr_t)ppc_lfd_ps1_fr13, (uintptr_t)ppc_lfd_ps1_fr14, (uintptr_t)ppc_lfd_ps1_fr15,
	(uintptr_t)ppc_lfd_ps1_fr16, (uintptr_t)ppc_lfd_ps1_fr17, (uintptr_t)ppc_lfd_ps1_fr18, (uintptr_t)ppc_lfd_ps1_fr19,
	(uintptr_t)ppc_lfd_ps1_fr20, (uintptr_t)ppc_lfd_ps1_fr21, (uintptr_t)ppc_lfd_ps1_fr22, (uintptr_t)ppc_lfd_ps1_fr23,
	(uintptr_t)ppc_lfd_ps1_fr24, (uintptr_t)ppc_lfd_ps1_fr25, (uintptr_t)ppc_lfd_ps1_fr26, (uintptr_t)ppc_lfd_ps1_fr27,
	(uintptr_t)ppc_lfd_ps1_fr28, (uintptr_t)ppc_lfd_ps1_fr29, (uintptr_t)ppc_lfd_ps1_fr30, (uintptr_t)ppc_lfd_ps1_fr31,
};

// After PS estimate (suite excess-range ps_rsqrte):
// - Double-format input (low 29 fraction bits set): keep full result exponent,
//   but truncate the mantissa to 23 bits ("mantissa of ps0 is rounded to single
//   precision, but the exponent keeps its full double-precision range") →
//   rsqrte(HUGE) 0x1FF00008_2C000000 becomes 0x1FF00008_20000000.
// - Single-format input: fold into the single domain; when freescale single exp
//   is tiny (< 96), force unbiased 0 ("ps1's exponent gets reset to 0").
ATTR_MS_ABI double ppc_ps_fold_estimate(double input, double result)
{
	uint64 in, res;
	std::memcpy(&in, &input, sizeof(in));
	std::memcpy(&res, &result, sizeof(res));
	const uint64 absIn = in & 0x7FFFFFFFFFFFFFFFULL;
	const uint64 absRes = res & 0x7FFFFFFFFFFFFFFFULL;
	if (absIn == 0 || absIn >= 0x7FF0000000000000ULL)
		return result;
	if (absRes == 0 || absRes >= 0x7FF0000000000000ULL)
		return result;

	if ((in & ((1ull << 29) - 1ull)) != 0)
	{
		// Double-format: truncate result mantissa, keep exp.
		const uint64 t = res & ~((1ull << 29) - 1ull);
		double out;
		std::memcpy(&out, &t, sizeof(out));
		return out;
	}

	uint32 s = ConvertToSingleNoFTZ(res);
	const uint32 sexp = (s >> 23) & 0xFFu;
	if (sexp != 0 && sexp < 96)
		s = (s & 0x807FFFFFu) | (0x7Fu << 23);
	const uint64 e = ConvertToDoubleNoFTZ(s);
	double out;
	std::memcpy(&out, &e, sizeof(out));
	return out;
}

ATTR_MS_ABI double ppc_ps_pack_arith(double r)
{
	const uint64 b = *(const uint64*)&r;
	if ((b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
		return r;
	return ppc_ps_quantize(r);
}

void PPCInterpreter_FADD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fadd(hCPU->fpr[frA].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FDIV(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fdiv(hCPU->fpr[frA].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FSUB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frC==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fsub(hCPU->fpr[frA].fpr, hCPU->fpr[frB].fpr);

	PPCInterpreter_nextInstruction(hCPU);
}

void PPCInterpreter_FMUL(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();

	int frD, frA, frB, frC;
	PPC_OPC_TEMPL_A(Opcode, frD, frA, frB, frC);
	PPC_ASSERT(frB == 0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fmul(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr);

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
	// 750CL high word matches fctiw packing (suite mffs only checks the low word).
	hCPU->fpr[frD].guint = 0xFFF8000000000000ULL | (uint64)hCPU->fpscr;
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

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
	ppc_fpscr_recompute(hCPU->fpscr);
	PPCInterpreter_setRoundingModeFromFPSCR(hCPU);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

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
	// Source is the low 32 bits of frB (suite builds FPSCR via stw/lfd).
	const uint32 src = (uint32)hCPU->fpr[frB].guint;
	hCPU->fpscr = (src & FM) | (hCPU->fpscr & ~FM);
	ppc_fpscr_recompute(hCPU->fpscr);
	PPCInterpreter_setRoundingModeFromFPSCR(hCPU);
	if (Opcode & PPC_OPC_RC)
		ppc_fpscr_update_cr1(hCPU);

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
	PPC_ASSERT(frC==0);

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	double r = ppc_fdiv(hCPU->fpr[frA].fpr, hCPU->fpr[frB].fpr);
	if (!ppc_fma_was_suppressed())
	{
		const uint64 b = *(const uint64*)&r;
		if ((b & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL)
		{
			// Pack to single with FZ clear (same tininess case as fdiv).
			const uint64 fpcr = __arm_rsr64("fpcr");
			if (fpcr & (1ull << 24))
				__arm_wsr64("fpcr", fpcr & ~(1ull << 24));
			std::atomic_signal_fence(std::memory_order_seq_cst);
			volatile double vr = r;
			volatile float sf = (float)vr;
			volatile double out = (double)sf;
			std::atomic_signal_fence(std::memory_order_seq_cst);
			if (fpcr & (1ull << 24))
				__arm_wsr64("fpcr", fpcr);
			r = out;
		}
	}
	hCPU->fpr[frD].fpr = r;
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

	ppc_fma_bind_dest(hCPU->fpr[frD].fpr);
	hCPU->fpr[frD].fpr = ppc_fmuls(hCPU->fpr[frA].fpr, hCPU->fpr[frC].fpr);
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

// Standalone validation of the ARMv8 crypto-extension AES-128 sequences
// against FIPS-197 Appendix C.1 and a reference software implementation.
// Once green, the ARMV8_* routines below are transplanted into src/util/crypto/aes128.cpp.
#include <arm_neon.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

typedef uint8_t uint8;
typedef uint32_t uint32;

static const uint8 sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const uint8 Rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

// ---------------------------------------------------------------------------
// Candidate implementation (to be transplanted)
// ---------------------------------------------------------------------------

// FIPS-197 AES-128 key expansion -> 11 round keys of 16 bytes each.
static void ARMV8_AES128_ExpandKey(const uint8* key, uint8 rk[11][16])
{
	memcpy(rk[0], key, 16);
	for (uint32 r = 1; r < 11; r++)
	{
		const uint8* prev = rk[r - 1];
		uint8* cur = rk[r];
		// RotWord + SubWord + Rcon applied to the last word of the previous key
		cur[0] = prev[0] ^ sbox[prev[13]] ^ Rcon[r];
		cur[1] = prev[1] ^ sbox[prev[14]];
		cur[2] = prev[2] ^ sbox[prev[15]];
		cur[3] = prev[3] ^ sbox[prev[12]];
		for (uint32 i = 4; i < 16; i++)
			cur[i] = prev[i] ^ cur[i - 4];
	}
}

// AESE Vd,Vn == ShiftRows(SubBytes(Vd ^ Vn)) -- AddRoundKey happens FIRST.
// So round i consumes rk[i], and the schedule is used un-transformed.
static inline uint8x16_t ARMV8_EncryptBlock(uint8x16_t s, const uint8x16_t* rk)
{
	for (uint32 i = 0; i < 9; i++)
		s = vaesmcq_u8(vaeseq_u8(s, rk[i]));
	s = vaeseq_u8(s, rk[9]);
	return veorq_u8(s, rk[10]);
}

// AESD Vd,Vn == InvSubBytes(InvShiftRows(Vd ^ Vn)) -- AddRoundKey happens FIRST.
// AESIMC applies InvMixColumns.
//
// The straight inverse cipher does AddRoundKey(rk[r]) *before* InvMixColumns.
// Because InvMixColumns is linear, that reorders into the "equivalent inverse
// cipher": apply InvMixColumns to the state, then XOR a *pre-transformed* round
// key dk[r] = InvMixColumns(rk[r]). AESD's key operand is consumed before the
// inverse transforms, so the middle round keys must be pre-transformed.
// Using the plain schedule here is the classic way to get a plausible-but-wrong
// result -- it passes nothing.
static void ARMV8_BuildDecryptSchedule(const uint8x16_t* rk, uint8x16_t* dk)
{
	dk[0] = rk[0];
	dk[10] = rk[10];
	for (uint32 i = 1; i <= 9; i++)
		dk[i] = vaesimcq_u8(rk[i]);
}

static inline uint8x16_t ARMV8_DecryptBlock(uint8x16_t s, const uint8x16_t* dk)
{
	s = vaesdq_u8(s, dk[10]);
	for (uint32 i = 9; i >= 1; i--)
	{
		s = vaesimcq_u8(s);
		s = vaesdq_u8(s, dk[i]);
	}
	return veorq_u8(s, dk[0]);
}

static void armv8_ECB_encrypt(uint8* input, const uint8* key, uint8* output)
{
	uint8 rkb[11][16];
	ARMV8_AES128_ExpandKey(key, rkb);
	uint8x16_t rk[11];
	for (uint32 i = 0; i < 11; i++) rk[i] = vld1q_u8(rkb[i]);
	vst1q_u8(output, ARMV8_EncryptBlock(vld1q_u8(input), rk));
}

// CBC decrypt: the XOR chain is on the output side, so blocks decrypt in
// parallel. Process 4 at a time.
static void armv8_CBC_decrypt(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv)
{
	uint8 rkb[11][16];
	ARMV8_AES128_ExpandKey(key, rkb);
	uint8x16_t rk[11], dk[11];
	for (uint32 i = 0; i < 11; i++) rk[i] = vld1q_u8(rkb[i]);
	ARMV8_BuildDecryptSchedule(rk, dk);

	uint8x16_t chain = iv ? vld1q_u8(iv) : vdupq_n_u8(0);
	uint32 blocks = length / 16;
	uint32 b = 0;
	for (; b + 4 <= blocks; b += 4)
	{
		uint8x16_t c0 = vld1q_u8(input + (b + 0) * 16);
		uint8x16_t c1 = vld1q_u8(input + (b + 1) * 16);
		uint8x16_t c2 = vld1q_u8(input + (b + 2) * 16);
		uint8x16_t c3 = vld1q_u8(input + (b + 3) * 16);
		uint8x16_t d0 = ARMV8_DecryptBlock(c0, dk);
		uint8x16_t d1 = ARMV8_DecryptBlock(c1, dk);
		uint8x16_t d2 = ARMV8_DecryptBlock(c2, dk);
		uint8x16_t d3 = ARMV8_DecryptBlock(c3, dk);
		vst1q_u8(output + (b + 0) * 16, veorq_u8(d0, chain));
		vst1q_u8(output + (b + 1) * 16, veorq_u8(d1, c0));
		vst1q_u8(output + (b + 2) * 16, veorq_u8(d2, c1));
		vst1q_u8(output + (b + 3) * 16, veorq_u8(d3, c2));
		chain = c3;
	}
	for (; b < blocks; b++)
	{
		uint8x16_t c = vld1q_u8(input + b * 16);
		vst1q_u8(output + b * 16, veorq_u8(ARMV8_DecryptBlock(c, dk), chain));
		chain = c;
	}
}

// ---------------------------------------------------------------------------
// Reference software AES-128 (independent, FIPS-197 straight from the spec)
// ---------------------------------------------------------------------------
static uint8 xt(uint8 x) { return (uint8)((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }
static uint8 mul(uint8 a, uint8 b)
{
	uint8 r = 0;
	while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
	return r;
}
static void ref_encrypt(const uint8* in, const uint8 rk[11][16], uint8* out)
{
	uint8 s[16]; memcpy(s, in, 16);
	for (int i = 0; i < 16; i++) s[i] ^= rk[0][i];
	for (int round = 1; round <= 10; round++)
	{
		for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
		uint8 t[16];
		for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
		memcpy(s, t, 16);
		if (round != 10)
		{
			for (int c = 0; c < 4; c++)
			{
				uint8* p = s + c * 4;
				uint8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
				p[0] = (uint8)(mul(a0,2) ^ mul(a1,3) ^ a2 ^ a3);
				p[1] = (uint8)(a0 ^ mul(a1,2) ^ mul(a2,3) ^ a3);
				p[2] = (uint8)(a0 ^ a1 ^ mul(a2,2) ^ mul(a3,3));
				p[3] = (uint8)(mul(a0,3) ^ a1 ^ a2 ^ mul(a3,2));
			}
		}
		for (int i = 0; i < 16; i++) s[i] ^= rk[round][i];
	}
	memcpy(out, s, 16);
}

static void hex(const char* label, const uint8* p, int n)
{
	printf("%s", label);
	for (int i = 0; i < n; i++) printf("%02x", p[i]);
	printf("\n");
}

int main()
{
	int fail = 0;

	// --- FIPS-197 Appendix C.1 -------------------------------------------
	uint8 key[16], pt[16], expect[16], got[16];
	for (int i = 0; i < 16; i++) { key[i] = (uint8)i; pt[i] = (uint8)(i * 0x11); }
	static const uint8 c1[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
	                             0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
	memcpy(expect, c1, 16);
	armv8_ECB_encrypt(pt, key, got);
	hex("FIPS-197 expect : ", expect, 16);
	hex("FIPS-197 neon   : ", got, 16);
	if (memcmp(expect, got, 16) != 0) { printf("  *** ECB ENCRYPT MISMATCH\n"); fail++; }
	else printf("  ECB encrypt OK\n");

	// --- key expansion vs reference --------------------------------------
	{
		uint8 rk[11][16]; ARMV8_AES128_ExpandKey(key, rk);
		uint8 refout[16]; ref_encrypt(pt, rk, refout);
		if (memcmp(refout, c1, 16) != 0) { printf("  *** KEY EXPANSION MISMATCH\n"); fail++; }
		else printf("  key expansion OK\n");
	}

	// --- CBC decrypt round-trip over random data --------------------------
	{
		srandom(12345);
		for (int trial = 0; trial < 2000; trial++)
		{
			uint32 blocks = 1 + (random() % 9);
			uint32 len = blocks * 16;
			uint8 k[16], iv[16], plain[160], ct[160], back[160];
			for (int i = 0; i < 16; i++) { k[i] = random(); iv[i] = random(); }
			for (uint32 i = 0; i < len; i++) plain[i] = random();

			// encrypt with the reference, CBC
			uint8 rk[11][16]; ARMV8_AES128_ExpandKey(k, rk);
			uint8 chain[16]; memcpy(chain, iv, 16);
			for (uint32 b = 0; b < blocks; b++)
			{
				uint8 blk[16];
				for (int i = 0; i < 16; i++) blk[i] = plain[b*16+i] ^ chain[i];
				ref_encrypt(blk, rk, ct + b*16);
				memcpy(chain, ct + b*16, 16);
			}
			// decrypt with the NEON path
			armv8_CBC_decrypt(back, ct, len, k, iv);
			if (memcmp(back, plain, len) != 0)
			{
				printf("  *** CBC ROUND-TRIP MISMATCH (trial %d, %u blocks)\n", trial, blocks);
				hex("    plain: ", plain, 16);
				hex("    back : ", back, 16);
				fail++; break;
			}
		}
		if (!fail) printf("  CBC decrypt round-trip OK (2000 trials, 1-9 blocks)\n");
	}

	printf(fail ? "\nFAILED\n" : "\nALL PASS\n");
	return fail != 0;
}

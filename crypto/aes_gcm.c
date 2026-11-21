/*
 * AES-128-GCM with a 16-byte IV, forward cipher only (see aes_gcm.h).
 * GHASH uses an 8-bit table of multiples of H (Shoup's method), 4 KB.
 * This code is in the Public Domain
 */

#include <stdint.h>
#include "aes_gcm.h"

#pragma codeseg BANK5
#pragma constseg BANK5

__xdata struct gcm_t gcm;

static __code const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* Source byte of each state byte after ShiftRows; the state is column-major. */
static __code const uint8_t shift_src[16] = {
	0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11
};


#define XTIME(a) ((uint8_t)(((a) << 1) ^ (((a) & 0x80) ? 0x1b : 0)))

static __xdata uint8_t aes_t[16];

/* Encrypts gcm.blk in place with gcm.rk. Few locals: it runs in the 8-byte
 * overlay segment, so MixColumns reads aes_t instead of copying columns. */
void aes_encrypt_blk(void) __banked
{
	uint8_t i, r, t;

	for (i = 0; i < 16; i++)
		gcm.blk[i] ^= gcm.rk[i];
	for (r = 16; ; r += 16) {
		for (i = 0; i < 16; i++)
			aes_t[i] = sbox[gcm.blk[shift_src[i]]];
		if (r == 160)
			break;
		for (i = 0; i < 16; i += 4) {
			t = aes_t[i] ^ aes_t[i + 1] ^ aes_t[i + 2] ^ aes_t[i + 3];
			gcm.blk[i] = aes_t[i] ^ t ^ XTIME(aes_t[i] ^ aes_t[i + 1]) ^ gcm.rk[r + i];
			gcm.blk[i + 1] = aes_t[i + 1] ^ t ^ XTIME(aes_t[i + 1] ^ aes_t[i + 2]) ^ gcm.rk[r + i + 1];
			gcm.blk[i + 2] = aes_t[i + 2] ^ t ^ XTIME(aes_t[i + 2] ^ aes_t[i + 3]) ^ gcm.rk[r + i + 2];
			gcm.blk[i + 3] = aes_t[i + 3] ^ t ^ XTIME(aes_t[i + 3] ^ aes_t[i]) ^ gcm.rk[r + i + 3];
		}
	}
	for (i = 0; i < 16; i++)
		gcm.blk[i] = aes_t[i] ^ gcm.rk[160 + i];
}

/* gcm.x = gcm.x * H, a byte at a time from the last byte to the first */
static void ghash_mult(void)
{
	__xdata uint8_t *p;
	uint8_t i, k, r;

	for (k = 0; k < 16; k++)
		gcm.z[k] = 0;
	i = 16;
	do {
		i--;
		p = gcm.m[gcm.x[i]];
		r = gcm.z[15];
		for (k = 15; k; k--)
			gcm.z[k] = gcm.z[k - 1] ^ p[k];
		/* Reduce the byte r shifted out: r * 0xe1 lands in the top 16 bits. */
		gcm.z[0] = p[0] ^ r ^ (r >> 1) ^ (r >> 2) ^ (r >> 7);
		gcm.z[1] ^= (uint8_t)((r << 1) ^ (r << 6) ^ (r << 7));
	} while (i);
	for (k = 0; k < 16; k++)
		gcm.x[k] = gcm.z[k];
}

/* Round keys from the key, and a zero block in gcm.blk to make H with. Leaf
 * functions keep their locals in the overlay segment; the callers do not. */
static void aes_expand(__xdata const uint8_t *key)
{
	static __code const uint8_t rcon[10] = {
		0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
	};
	uint8_t i, j, t0, t1, t2, t3;

	for (i = 0; i < 16; i++) {
		gcm.rk[i] = key[i];
		gcm.blk[i] = 0;
	}
	for (i = 16, j = 0; i < 176; i += 4) {
		t0 = gcm.rk[i - 4];
		t1 = gcm.rk[i - 3];
		t2 = gcm.rk[i - 2];
		t3 = gcm.rk[i - 1];
		if (!(i & 15)) {
			j = t0;
			t0 = sbox[t1] ^ rcon[(i >> 4) - 1];
			t1 = sbox[t2];
			t2 = sbox[t3];
			t3 = sbox[j];
		}
		gcm.rk[i] = gcm.rk[i - 16] ^ t0;
		gcm.rk[i + 1] = gcm.rk[i - 15] ^ t1;
		gcm.rk[i + 2] = gcm.rk[i - 14] ^ t2;
		gcm.rk[i + 3] = gcm.rk[i - 13] ^ t3;
	}
}

/* The 8-bit table from H in gcm.blk: m[0x80] = H, m[0x40] = H * x, down to
 * m[0x01] = H * x^7, and every other entry the sum of those. */
static void ghash_table(void)
{
	uint8_t i, j, r;

	for (i = 0; i < 16; i++) {
		gcm.m[0][i] = 0;
		gcm.m[0x80][i] = gcm.blk[i];
	}
	for (j = 0x40; j; j >>= 1) {
		r = (gcm.m[j << 1][15] & 1) ? 0xe1 : 0;
		for (i = 15; i; i--)
			gcm.m[j][i] = (gcm.m[j << 1][i] >> 1) | (gcm.m[j << 1][i - 1] << 7);
		gcm.m[j][0] = (gcm.m[j << 1][0] >> 1) ^ r;
	}
	for (j = 2; j; j <<= 1)
		for (r = 1; r < j; r++)
			for (i = 0; i < 16; i++)
				gcm.m[j + r][i] = gcm.m[j][i] ^ gcm.m[r][i];
}

void gcm_setkey(__xdata const uint8_t * __xdata key) __banked
{
	aes_expand(key);
	aes_encrypt_blk();
	ghash_table();
}

void gcm_start(__xdata const uint8_t * __xdata iv, __xdata uint8_t decrypt) __banked
{
	uint8_t i;

	gcm.decrypt = decrypt;
	gcm.aad_len = 0;
	gcm.len = 0;
	/* A 16-byte IV is not 96 bits, so J0 = GHASH(IV || 0^64 || [128]_64). */
	for (i = 0; i < 16; i++)
		gcm.x[i] = iv[i];
	ghash_mult();
	gcm.x[15] ^= 0x80;
	ghash_mult();
	for (i = 0; i < 16; i++) {
		gcm.j0[i] = gcm.x[i];
		gcm.ctr[i] = gcm.x[i];
		gcm.x[i] = 0;
	}
}

/* The next counter block into gcm.blk; inc32 steps its last 4 bytes. */
static void ctr_next(void)
{
	uint8_t k;

	for (k = 15; k >= 12; k--)
		if (++gcm.ctr[k])
			break;
	for (k = 0; k < 16; k++)
		gcm.blk[k] = gcm.ctr[k];
}

/* XORs the keystream in gcm.blk into n bytes of p, hashing the ciphertext. */
static void ctr_xor(__xdata uint8_t *p, uint8_t n)
{
	uint8_t k, c;

	for (k = 0; k < n; k++) {
		c = p[k];
		if (!gcm.decrypt)
			c ^= gcm.blk[k];
		gcm.x[k] ^= c;
		p[k] ^= gcm.blk[k];
	}
}

void gcm_crypt(__xdata uint8_t * __xdata p, __xdata uint16_t len) __banked
{
	__xdata uint8_t n;

	gcm.len += len;
	while (len) {
		ctr_next();
		aes_encrypt_blk();
		n = len < 16 ? len : 16;
		ctr_xor(p, n);
		ghash_mult();
		p += n;
		len -= n;
	}
}

/* With a zero keystream, ctr_xor() only hashes: p is rewritten unchanged. */
void gcm_aad(__xdata uint8_t * __xdata p, __xdata uint16_t len) __banked
{
	__xdata uint8_t n;

	gcm.aad_len = len;
	for (n = 0; n < 16; n++)
		gcm.blk[n] = 0;
	while (len) {
		n = len < 16 ? len : 16;
		ctr_xor(p, n);
		ghash_mult();
		p += n;
		len -= n;
	}
}

void gcm_finish(void) __banked
{
	uint8_t k;

	/* [len(A)]_64 || [len(C)]_64 in bits: A < 8 KB fits 2 bytes, C 3 bytes. */
	gcm.x[6] ^= gcm.aad_len >> 5;
	gcm.x[7] ^= gcm.aad_len << 3;
	gcm.x[13] ^= gcm.len >> 13;
	gcm.x[14] ^= gcm.len >> 5;
	gcm.x[15] ^= gcm.len << 3;
	ghash_mult();
	for (k = 0; k < 16; k++)
		gcm.blk[k] = gcm.j0[k];
	aes_encrypt_blk();
	for (k = 0; k < 16; k++)
		gcm.blk[k] ^= gcm.x[k];
}

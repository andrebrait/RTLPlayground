#ifndef _AES_GCM_H_
#define _AES_GCM_H_

#include <stdint.h>

/*
 * AES-128-GCM with a 16-byte IV, the variant UniFi inform packets use. Only
 * the forward cipher is needed: GCM decrypts with the same keystream.
 *
 * One message at a time:
 *   gcm_setkey(key);              once per key; it builds a 4 KB table and
 *                                 stalls about 30 ms, more than a 20 ms
 *                                 LACP tick, so call it where that is safe
 *   gcm_start(iv, decrypt);
 *   gcm_aad(aad, len);            all of the AAD, under 8 KB, in one call,
 *                                 or none; the buffer is written back unchanged
 *   gcm_crypt(p, len);            in place; every call but the last a
 *                                 multiple of 16 bytes, so it can be stepped
 *   gcm_finish();                 the tag is then in gcm.blk
 *
 * A message is at most 65535 bytes. Decryption writes plaintext into the
 * buffer before the tag is known: use none of it until gcm_finish() and the
 * tag compare pass. gcm_selftest() loads its own key and ends any message.
 */

struct gcm_t {
	uint8_t rk[176];	/* AES-128 round keys */
	uint8_t m[256][16];	/* m[n] = n * H, 8-bit table */
	uint8_t j0[16];		/* pre-counter block */
	uint8_t ctr[16];
	uint8_t blk[16];	/* AES block in place; the tag after gcm_finish */
	uint8_t x[16];		/* GHASH accumulator */
	uint8_t z[16];		/* multiplication scratch */
	uint16_t aad_len;
	uint16_t len;
	uint8_t decrypt;
};

extern __xdata struct gcm_t gcm;

void gcm_setkey(__xdata const uint8_t * __xdata key) __banked;
void gcm_start(__xdata const uint8_t * __xdata iv, __xdata uint8_t decrypt) __banked;
void gcm_aad(__xdata uint8_t * __xdata p, __xdata uint16_t len) __banked;
void gcm_crypt(__xdata uint8_t * __xdata p, __xdata uint16_t len) __banked;
void gcm_finish(void) __banked;
void aes_encrypt_blk(void) __banked;
#ifdef UNIFI
void gcm_selftest(void) __banked;
#endif

#endif

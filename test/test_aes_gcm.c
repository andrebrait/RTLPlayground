/* crypto/aes_gcm.c against Go's AES-128-GCM (test/gcm_vectors.h) and a real
 * UniFi controller reply (crypto/gcm_kat.h): encrypt and decrypt, in one call
 * and in 16- and 32-byte steps as the main loop will run it. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crypto/aes_gcm.c"
#include "crypto/gcm_kat.h"
#include "gcm_vectors.h"

static int failures;

static void check(int ok, const char *what, int i, unsigned step)
{
	if (!ok) {
		failures++;
		printf("FAIL %s: vector %d, step %u\n", what, i, step);
	}
}

/* Runs one message through the stepped API; returns 1 if the tag matches. */
static int run(const uint8_t *key, const uint8_t *iv, const uint8_t *aad,
	       uint8_t *buf, uint16_t len, uint8_t decrypt, unsigned step,
	       const uint8_t *tag)
{
	uint8_t k[16], v[16], a[40];
	uint16_t off, n;

	memcpy(k, key, 16);
	memcpy(v, iv, 16);
	memcpy(a, aad, 40);
	gcm_setkey(k);
	gcm_start(v, decrypt);
	gcm_aad(a, 40);
	for (off = 0; off < len; off += n) {
		n = len - off;
		if (n > step)
			n = step;
		gcm_crypt(buf + off, n);
	}
	gcm_finish();
	return !memcmp(gcm.blk, tag, 16);
}

int main(void)
{
	static const unsigned steps[] = { 16, 32, 0xffff };
	static uint8_t buf[2048];
	unsigned s, i;

	for (i = 0; i < sizeof(gcm_vecs) / sizeof(gcm_vecs[0]); i++) {
		const struct gcm_vec *t = gcm_vecs[i];

		for (s = 0; s < 3; s++) {
			memcpy(buf, t->pt, t->len);
			check(run(t->key, t->iv, t->aad, buf, t->len, 0, steps[s], t->tag),
			      "encrypt tag", i, steps[s]);
			check(!memcmp(buf, t->ct, t->len), "ciphertext", i, steps[s]);
			check(run(t->key, t->iv, t->aad, buf, t->len, 1, steps[s], t->tag),
			      "decrypt tag", i, steps[s]);
			check(!memcmp(buf, t->pt, t->len), "plaintext", i, steps[s]);
		}
	}
	printf("%s AES-128-GCM: %u Go vectors, 0 to 1500 bytes, 16-byte IV, 40-byte AAD\n",
	       failures ? "FAIL" : "PASS", i);

	for (s = 0; s < 3; s++) {
		memcpy(buf, gcm_kat_packet + 40, GCM_KAT_PLAIN_LEN);
		check(run(gcm_kat_key, gcm_kat_packet + 16, gcm_kat_packet, buf,
			  GCM_KAT_PLAIN_LEN, 1, steps[s], gcm_kat_packet + 40 + GCM_KAT_PLAIN_LEN),
		      "controller tag", -1, steps[s]);
		check(!memcmp(buf, gcm_kat_plain, GCM_KAT_PLAIN_LEN), "controller plaintext", -1, steps[s]);
	}
	/* A flipped ciphertext bit must fail the tag. */
	memcpy(buf, gcm_kat_packet + 40, GCM_KAT_PLAIN_LEN);
	buf[100] ^= 1;
	check(!run(gcm_kat_key, gcm_kat_packet + 16, gcm_kat_packet, buf,
		   GCM_KAT_PLAIN_LEN, 1, 32, gcm_kat_packet + 40 + GCM_KAT_PLAIN_LEN),
	      "tampered packet accepted", -1, 32);
	printf("%s AES-128-GCM: UniFi controller reply decrypts and verifies; a flipped bit fails\n",
	       failures ? "FAIL" : "PASS");
	return failures != 0;
}

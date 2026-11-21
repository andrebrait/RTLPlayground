/*
 * unifi selftest: checks AES-128-GCM against a captured UniFi controller
 * reply and times the cipher on this switch.
 * This code is in the Public Domain
 */

#include <stdint.h>
#include "rtl837x_common.h"
#include "aes_gcm.h"

#pragma codeseg BANK5
#pragma constseg BANK5

#include "gcm_kat.h"

/* The first plaintext block of the KAT reply. */
static __code const char kat_head[] = "{\"_type\":\"setpar";

extern volatile __xdata uint32_t ticks;

static __xdata uint8_t st_buf[40];
static __xdata uint16_t st_t0;

static uint16_t st_ticks(void)
{
	uint16_t t;

	do {
		t = ticks;
	} while (t != (uint16_t)ticks);
	return t;
}

/* Copies n bytes of the KAT packet from offset off into st_buf. */
static void st_load(__xdata uint16_t off, __xdata uint8_t n)
{
	uint8_t i;

	for (i = 0; i < n; i++)
		st_buf[i] = gcm_kat_packet[off + i];
}

static void st_ms(__code const char *what)
{
	print_string(what);
	itoa_short((st_ticks() - st_t0) * (1000 / SYS_TICK_HZ));
	print_string(" ms");
}

void gcm_selftest(void) __banked
{
	__xdata uint16_t off;
	__xdata uint8_t i, ok;

	for (i = 0; i < 16; i++)
		st_buf[i] = gcm_kat_key[i];
	st_t0 = st_ticks();
	gcm_setkey(st_buf);
	st_ms("Key setup: ");
	write_char('\n');
	st_load(16, 16);
	gcm_start(st_buf, 1);
	st_load(0, 40);
	gcm_aad(st_buf, 40);
	ok = 1;
	for (off = 0; off < GCM_KAT_PLAIN_LEN; off += 32) {
		i = GCM_KAT_PLAIN_LEN - off < 32 ? GCM_KAT_PLAIN_LEN - off : 32;
		st_load(40 + off, i);
		gcm_crypt(st_buf, i);
		if (!off)
			for (i = 0; i < 16; i++)
				if (st_buf[i] != kat_head[i])
					ok = 0;
	}
	gcm_finish();
	st_load(40 + GCM_KAT_PLAIN_LEN, 16);
	for (i = 0; i < 16; i++)
		if (gcm.blk[i] != st_buf[i])
			ok = 0;
	print_string(ok ? "AES-128-GCM: controller reply verifies\n"
			: "AES-128-GCM: controller reply FAILS\n");

	st_t0 = st_ticks();
	for (i = 0; i < 64; i++)
		aes_encrypt_blk();
	st_ms("64 AES blocks: ");
	st_t0 = st_ticks();
	for (i = 0; i < 64; i++)
		gcm_crypt(st_buf, 16);
	st_ms(", 64 GCM blocks: ");
	write_char('\n');
}

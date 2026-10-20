/*
 * Console output and input. Output formats follow rtlplayground.c (the 8051 reference): a
 * newline goes out as CR LF, the prompt is "\n> ". Input reaches cmd_editor.c through the
 * same ring the serial ISR fills.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmd_editor.h"
#include "cmd_parser.h"
#include "linux.h"

static int out_fd = STDOUT_FILENO;
static uint8_t out_buf[1024];
static size_t out_len;

void console_flush(void)
{
	size_t done = 0;

	while (done < out_len) {
		ssize_t n = write(out_fd, out_buf + done, out_len - done);
		if (n <= 0)
			break;	/* the reader is gone: there is nobody to tell */
		done += n;
	}
	out_len = 0;
}

void console_set_fd(int fd)
{
	console_flush();
	out_fd = fd;
}

/* Output of a command that came over HTTP or the CLI socket: it answers that client only. */
static struct {
	char *buf;
	size_t len, cap;
	bool on, truncated;
} cap;

void console_capture_begin(void)
{
	cap.len = 0;
	cap.truncated = false;
	cap.on = true;
}

const char *console_capture_end(size_t *len, bool *truncated)
{
	cap.on = false;
	*len = cap.len;
	*truncated = cap.truncated;
	return cap.buf;
}

static void capture_char(char c)
{
	if (cap.len == cap.cap) {
		size_t ncap = cap.cap ? cap.cap * 2 : 1024;
		char *nbuf = cap.len < CAPTURE_MAX ? realloc(cap.buf, ncap) : NULL;

		if (!nbuf) {
			cap.truncated = true;
			return;
		}
		cap.buf = nbuf;
		cap.cap = ncap;
	}
	cap.buf[cap.len++] = c;
}

void write_char(char c)
{
	if (cap.on) {
		capture_char(c);
		return;
	}
	if (out_len + 2 > sizeof out_buf)
		console_flush();
	if (c == '\n')
		out_buf[out_len++] = '\r';
	out_buf[out_len++] = c;
}

void print_string(const char *p)
{
	while (*p)
		write_char(*p++);
}

void print_string_x(char *p)
{
	print_string(p);
}

void print_byte(uint8_t a)
{
	static const char digits[] = "0123456789abcdef";

	write_char(digits[a >> 4]);
	write_char(digits[a & 0xf]);
}

void print_short(uint16_t a)
{
	print_string("0x");
	print_byte(a >> 8);
	print_byte(a);
}

void print_long(uint32_t a)
{
	print_string("0x");
	print_byte(a >> 24);
	print_byte(a >> 16);
	print_byte(a >> 8);
	print_byte(a);
}

void itoa_short(uint16_t v)
{
	char digits[5];
	int n = 0;

	do {
		digits[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		write_char(digits[--n]);
}

void itoa(uint8_t v)
{
	itoa_short(v);
}

void print_cmd_prompt(void)
{
	print_string("\n> ");
}

// Print the physical port of a logical port number.
void print_phys_port(uint8_t port)
{
	if (port < CPU_PORT)
		write_char(machine.log_to_phys_port[port] + '0');
	else if (port == CPU_PORT)
		print_string("CPU");
	else {
		print_string("UNKNOWN ");
		write_char(port + '0');
	}
}

uint16_t strtox(uint8_t *dst, const char *s)
{
	size_t n = strlen(s);

	memcpy(dst, s, n + 1);
	return n;
}

/* One command, as the 8051 main loop runs it once the editor flags a complete line. */
static void run_command(void)
{
	cmd_available = 0;
	cmd_tokenize();
	if (err_status == ERR_OK)
		cmd_parser();
	auth_take_plain();
	print_cmd_prompt();
}

/* Pushes bytes into the editor's ring and lets it consume them. A command runs before the
 * next byte is pushed: the editor would otherwise type into cmd_buffer while it is parsed. */
static void push(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		sbuf[sbuf_ptr] = p[i];
		sbuf_ptr = (sbuf_ptr + 1) & SBUF_MASK;
	}
	cmd_edit();
	while (cmd_available) {
		run_command();
		cmd_edit();
	}
}

/* cmd_edit() waits for the rest of an escape sequence by returning without consuming it, and
 * spins forever on one it does not know. So the editor only ever sees whole sequences it
 * handles (ESC [ A/B/C/D, ESC [ 3 ~); every other escape sequence is swallowed here:
 *   ESC [ <parameter/intermediate bytes 0x20-0x3f> <final byte 0x40-0x7e>   (CSI; only the first CSI_MAX bytes are kept)
 *   ESC O <one byte>                                                         (SS3, e.g. F1..F4)
 *   ESC <printable>                                                          (Alt-key)
 * A control byte (CR, LF, ...) inside or right after an escape ends it and is then handled as
 * typed, so ESC followed by Enter still runs the line. */
#define CSI_MAX 16

enum esc_state { ESC_NONE, ESC_START, ESC_CSI, ESC_SS3 };

void console_feed(const uint8_t *buf, size_t len)
{
	static uint8_t seq[CSI_MAX];
	static size_t seq_n;
	static enum esc_state state;

	for (; len; buf++, len--) {
		uint8_t c = *buf;

		if (c == '\033') {	/* starts a sequence, whatever was pending is dead */
			seq[0] = c;
			seq_n = 1;
			state = ESC_START;
			continue;
		}
		switch (state) {
		case ESC_NONE:
			push(&c, 1);
			continue;
		case ESC_START:
			if (c == '[') {
				seq[seq_n++] = c;
				state = ESC_CSI;
				continue;
			}
			state = c == 'O' ? ESC_SS3 : ESC_NONE;
			break;
		case ESC_SS3:
			state = ESC_NONE;
			break;
		case ESC_CSI:
			/* seq_n keeps counting past CSI_MAX without storing: a sequence that long is
			 * never one the editor knows, but it still ends only at its final byte. */
			if (c >= 0x20 && c <= 0x3f) {
				if (seq_n < CSI_MAX)
					seq[seq_n] = c;
				seq_n++;
				continue;
			}
			state = ESC_NONE;
			if (c >= 0x40 && c <= 0x7e) {
				if (seq_n < CSI_MAX)
					seq[seq_n] = c;
				seq_n++;
				if ((seq_n == 3 && c >= 'A' && c <= 'D') || (seq_n == 4 && seq[2] == '3' && c == '~'))
					push(seq, seq_n);
				continue;
			}
			break;
		}
		/* Here the byte ended the escape without belonging to it: a printable one is part of
		 * a sequence we drop (ESC O x, ESC x), a control byte is typed input. */
		if (c < 0x20 || c == 0x7f)
			push(&c, 1);
	}
}

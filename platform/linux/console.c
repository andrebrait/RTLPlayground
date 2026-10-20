/*
 * Console output and input. Output formats follow rtlplayground.c (the 8051 reference): a
 * newline goes out as CR LF, the prompt is "\n> ". Input reaches cmd_editor.c through the
 * same ring the serial ISR fills.
 */
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

void write_char(char c)
{
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
	print_cmd_prompt();
}

/* Pushes bytes into the editor's ring and lets it consume them. cmd_edit() waits for the
 * rest of an escape sequence by returning without consuming it, and spins forever on one it
 * does not know, so only whole sequences the editor handles (cursor keys, Delete) are
 * pushed, together; any other escape sequence is dropped here. A command runs before the next
 * byte is pushed: the editor would otherwise type into cmd_buffer while it is being parsed. */
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

void console_feed(const uint8_t *buf, size_t len)
{
	static uint8_t esc[4];
	static uint8_t esc_n;

	for (; len; buf++, len--) {
		uint8_t c = *buf;

		if (esc_n == 0 && c != '\033') {
			push(&c, 1);
			continue;
		}
		if (c == '\033')
			esc_n = 0;	/* a new sequence starts, whatever was pending is dead */
		esc[esc_n++] = c;
		if (esc_n == 2 && c != '[')
			esc_n = 0;	/* ESC and a key: not a CSI sequence, drop it */
		else if (esc_n >= 3 && c >= 0x40 && c <= 0x7e) {	/* the final byte of the CSI */
			if ((esc_n == 3 && c >= 'A' && c <= 'D') || (esc_n == 4 && esc[2] == '3' && c == '~'))
				push(esc, esc_n);
			esc_n = 0;
		} else if (esc_n == 4)
			esc_n = 0;	/* longer than any sequence the editor knows */
	}
}

#ifndef UTIL_H
#define UTIL_H

	#include <stdint.h>
	#include <stdio.h>
	#include <stdlib.h>

	/* Macros. */
	#define expect_char(c,buf,len) \
		do { \
			if ((c) != *(buf)) { \
				errw("Expected '%c', got '%c'\n", (c), *(buf)); \
				send_gdb_error(); \
				return (-1); \
			} \
			buf++; \
			len--; \
		} while(0)

	#define send_serial_byte(b) \
		do { \
			uint8_t byte = (b); \
			send_all(serial_fd, &byte, 1, 0); \
		} while(0)

	#define send_serial_word(w) \
		do { \
			uint16_t word = (w); \
			send_all(serial_fd, &word, 2, 0); \
		} while(0)

	#define send_serial_dword(dw) \
		do { \
			uint32_t dword = (dw); \
			send_all(serial_fd, &dword, 4, 0); \
		} while(0)



	#define ABS(N) (((N)<0)?(-(N)):(N))
	#define MIN(x, y) ((x) < (y) ? (x) : (y))

	/**/
	#define errx(...) \
		do { \
			fprintf(stderr, __VA_ARGS__); \
			exit(1); \
		} while (0)

	#define errw(...) \
		do { \
			fprintf(stderr, __VA_ARGS__); \
			return 1; \
		} while (0)

	#define LOG_CMD_REC(...) \
		do { \
			fprintf(stderr, __VA_ARGS__); \
		} while (0)

	extern char *encode_hex(const char *data, size_t len);
	extern char *decode_hex(const char *data, size_t len);
	extern uint32_t read_int(const char *buff, size_t *len,
		const char **endptr);
	extern uint32_t simple_read_int(const char *buf, size_t len);

#endif /* UTIL_H */

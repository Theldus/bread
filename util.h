#ifndef UTIL_H
#define UTIL_H

	#include <stdint.h>
	#include <stdio.h>
	#include <stdlib.h>

	/* Macros. */

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
	extern uint32_t str2hex(const char *buff, size_t len,
		const char **endptr);

#endif /* UTIL_H */

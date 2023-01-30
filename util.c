#include <ctype.h>
#include <stdlib.h>

#include "util.h"

/**
 *
 */
static char  *gbuffer      = NULL;
static size_t gbuffer_size = 0;

/**
 *
 */
static void increase_buffer(size_t new_size)
{
	char *tmp;

	if (gbuffer_size < new_size)
	{
		tmp = realloc(gbuffer, new_size);
		if (!tmp)
			errx("Unable to allocate %zu bytes!\n", new_size);
		gbuffer = tmp;
		gbuffer_size = new_size;
	}
}

/**
 *
 */
static inline char to_digit(int nibble)
{
	static const char digits[] = "0123456789abcdef";
	return (digits[nibble]);
}

/**
 *
 */
static inline int to_value(int ch)
{
	int c = tolower(ch);

	if (c >= '0' && c <= '9')
		return (c - '0');
	else if (c >= 'a' && c <= 'f')
		return (0xA + c - 'a');
	else
		return (-1);
}

/**
 *
 */
char *encode_hex(const char *data, size_t len)
{
	char *tmp;
	int i;

	increase_buffer(len * 2);

	for (i = 0, tmp = gbuffer; i < len; i++)
	{
		*tmp++ = to_digit((data[i] >> 4) & 0xF);
		*tmp++ = to_digit((data[i]     ) & 0xF);
	}

	return (gbuffer);
}

/**
 *
 */
char *decode_hex(const char *data, size_t len)
{
	char *ptr;
	int i;

	increase_buffer(len);

	for (i = 0, ptr = gbuffer; i < len * 2; i += 2, ptr++)
	{
		*ptr =  to_value(data[i]);
		*ptr <<= 4;
		*ptr |= to_value(data[i+1]);
	}

	return (gbuffer);
}

/**
 *
 */
uint32_t read_int(const char *buff, size_t *len,
	const char **endptr)
{
	int32_t ret;
	char c;
	int v;

	ret = 0;

	for (int i = 0; i < *len; i++)
	{
		c = tolower(buff[i]);

		if (c >= '0' && c <= '9')
			v = (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (c - 'a' + 10);
		else
		{
			if (endptr)
			{
				*endptr = buff + i;
				*len = *len - i;
			}
			goto out;
		}
		ret = ret * 16 + v;
	}

out:
	return (ret);
}

/**
 *
 */
uint32_t simple_read_int(const char *buf, size_t len)
{
	size_t l = len;
	return read_int(buf, &l, NULL);
}

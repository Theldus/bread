/*
 * MIT License
 *
 * Copyright (c) 2023 Davidson Francis <davidsondfgl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <ctype.h>
#include <stdlib.h>

#include "util.h"

/**
 * Global buffer, responsible to handle dynamic allocations
 * from/to GDB messages.
 */
static char  *gbuffer      = NULL;
static size_t gbuffer_size = 0;

/**
 * @brief Increase the global buffer size to a new value
 * if needed.
 *
 * @param new_size New buffer size.
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
 * @brief For a given nibble, to converts to its
 * ascii representative form, ie: 10 -> a.
 *
 * @param nibble Nibble input value to be converted.
 *
 * @return Returns the converted value.
 */
static inline char to_digit(int nibble)
{
	static const char digits[] = "0123456789abcdef";
	return (digits[nibble]);
}

/**
 * @brief For a given nibble (in its char form),
 * convert to the decimal representation, i.e:
 * 'b' -> 11.
 *
 * @param ch Char nibble to be converted.
 *
 * @return Returns the converted value.
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
 * @brief Encodes a binary data inside @p data to its
 * representative form in ascii hex value.
 *
 * @param data Data to be encoded in ascii-hex form.
 * @param len Length of @p data.
 *
 * @return Returns a buffer containing the encoded
 * buffer.
 *
 * @note Please note that the size of the output
 * buffer is twice bigger than the input buffer.
 */
char *encode_hex(const char *data, size_t len)
{
	char *tmp;
	size_t i;

	increase_buffer(len * 2);

	for (i = 0, tmp = gbuffer; i < len; i++)
	{
		*tmp++ = to_digit((data[i] >> 4) & 0xF);
		*tmp++ = to_digit((data[i]     ) & 0xF);
	}

	return (gbuffer);
}

/**
 * @brief Converts an input buffer containing an ascii
 * hex-value representation into the equivalent binary
 * form.
 *
 * @param data Input buffer to be decoded to binary.
 * @param len Input buffer length.
 *
 * @return Returns the buffer containing the binary
 * representation of the data.
 */
char *decode_hex(const char *data, size_t len)
{
	char *ptr;
	size_t i;

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
 * @brief Reads a given integer encoded in hex
 * and returns it.
 *
 * @param buffer Buffer containing the integer to be
 *               read.
 * @param len    Buffer length. This variable is updated.
 *
 * @param endptr If not NULL, the position for the first
 *               non-hex digit character is saved.
 *
 * @param base   Number base, whether 10 or 16.
 *
 * @return Returns the integer read.
 */
uint32_t read_int(const char *buff, size_t *len,
	const char **endptr, int base)
{
	int32_t ret;
	char c;
	int v;

	ret = 0;

	for (size_t i = 0; i < *len; i++)
	{
		c = tolower(buff[i]);

		if (c >= '0' && c <= '9')
			v = (c - '0');
		else if (base == 16 && (c >= 'a' && c <= 'f'))
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
		ret = ret * base + v;
	}

out:
	return (ret);
}

/**
 * @brief Same behavior as read_int(), but do not
 * updates @len nor returns where next non-digit
 * char is.
 *
 * @param buf Buffer to be read.
 * @param len Buffer length.
 * @param base Number base, whether 10 or 16.
 *
 * @return Returns the integer read.
 */
uint32_t simple_read_int(const char *buf, size_t len, int base)
{
	size_t l = len;
	return read_int(buf, &l, NULL, base);
}

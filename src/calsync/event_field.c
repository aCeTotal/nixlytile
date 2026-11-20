/* Event text, colors, times, hashes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calsync.h"

#define LEN(a)          (sizeof(a) / sizeof *(a))
#define DEC_DIGITS      "0123456789"
#define RGBA_DIGITS     8
#define BITS_PER_DIGIT  4
#define ASCII_DEL       0x7f
#define TM_YEAR_BASE    1900
#define SECS_PER_HOUR   3600
#define SECS_PER_MIN    60
#define RFC3339_SHAPE   "dddd-dd-ddTdd:dd:dd"
#define ZONE_SHAPE      "dd:dd"
#define FNV_OFFSET      0xcbf29ce484222325ull
#define FNV_PRIME       0x100000001b3ull
#define UTF8_CONT(c)    (((unsigned char)(c) & 0xc0) == 0x80)

static const uint32_t palette[] = {
	0x5b9cf6, 0x34c08b, 0xf2a43a, 0xe5677d,
	0xa78bfa, 0x2fc6c8, 0xf2789f, 0x8fb339,
};

/* Drop a split UTF-8 sequence. */
static size_t
utf8_floor(const char *s, size_t n, char next)
{
	if (!UTF8_CONT(next))
		return n;
	while (n > 0 && UTF8_CONT(s[n - 1]))
		n--;
	return n > 0 ? n - 1 : 0;
}

void
event_text(char *dst, size_t len, const char *s)
{
	size_t n = 0;

	if (!s)
		s = "";
	for (; *s && n + 1 < len; s++) {
		unsigned char ch = (unsigned char)*s;
		int blank = ch <= ' ' || ch == ASCII_DEL;

		if (blank && (n == 0 || dst[n - 1] == ' '))
			continue;
		dst[n++] = blank ? ' ' : (char)ch;
	}
	n = utf8_floor(dst, n, *s);
	while (n > 0 && dst[n - 1] == ' ')
		n--;
	dst[n] = '\0';
}

uint32_t
palette_color(const char *name)
{
	return palette[fnv64(name) % LEN(palette)];
}

int
parse_color(const char *s, uint32_t *rgb)
{
	size_t n;

	if (!s || s[0] != '#')
		return -1;
	n = strlen(s + 1);
	if ((n != RGB_DIGITS && n != RGBA_DIGITS) || strspn(s + 1, HEX_DIGITS) != n)
		return -1;
	*rgb = (uint32_t)(strtoul(s + 1, NULL, 16)
			>> (n - RGB_DIGITS) * BITS_PER_DIGIT);
	return 0;
}

int64_t
local_midnight(int y, int m, int d)
{
	struct tm tm = {
		.tm_year = y - TM_YEAR_BASE,
		.tm_mon = m - 1,
		.tm_mday = d,
		.tm_isdst = -1,
	};

	return (int64_t)mktime(&tm);
}

/* Digits where shape has 'd'. */
static int
has_shape(const char *s, const char *shape)
{
	for (; *shape; s++, shape++)
		if (*shape == 'd' ? !isdigit((unsigned char)*s) : *s != *shape)
			return 0;
	return 1;
}

/* UTC offset, else INT64_MIN. */
static int64_t
zone_offset(const char *z)
{
	int h, m;
	int64_t off;

	if (!z[0] || ((z[0] == 'Z' || z[0] == 'z') && !z[1]))
		return 0;
	if ((z[0] != '+' && z[0] != '-') || !has_shape(z + 1, ZONE_SHAPE)
			|| z[sizeof ZONE_SHAPE])
		return INT64_MIN;
	sscanf(z + 1, "%2d:%2d", &h, &m);
	off = (int64_t)h * SECS_PER_HOUR + (int64_t)m * SECS_PER_MIN;
	return z[0] == '-' ? -off : off;
}

int64_t
parse_rfc3339(const char *s)
{
	struct tm tm = { 0 };
	const char *z;
	int64_t off;

	if (!s || !has_shape(s, RFC3339_SHAPE))
		return INT64_MIN;
	z = s + sizeof RFC3339_SHAPE - 1;
	if (*z == '.')
		z += 1 + strspn(z + 1, DEC_DIGITS);
	off = zone_offset(z);
	if (off == INT64_MIN)
		return INT64_MIN;
	sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon,
			&tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
	tm.tm_year -= TM_YEAR_BASE;
	tm.tm_mon--;
	return (int64_t)timegm(&tm) - off;
}

uint64_t
fnv64(const char *s)
{
	uint64_t h = FNV_OFFSET;

	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * FNV_PRIME;
	return h;
}

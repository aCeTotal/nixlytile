/* Civil dates as day numbers. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#include <limits.h>
#include <strings.h>

#define DAYS_400Y    146097
#define DAYS_TO_1970 719468
#define WEEK         7
#define CENTURY      100
#define TWO_DIGITS   100
#define Y2K          2000
#define INPUT_MAX    64
#define SHIFT_MAX    (YEAR_MAX * 366)
#define YEAR_DIGITS  4

typedef struct {
	const char *word;
	int offset;
} DayWord;

static const char *const weekday_names[WEEK] = {
	"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
	"Sunday",
};
static const char *const month_names[MONTHS] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December",
};
static const DayWord day_words[] = {
	{ "today", 0 }, { "i dag", 0 }, { "idag", 0 },
	{ "tomorrow", 1 }, { "i morgen", 1 }, { "imorgen", 1 },
	{ "yesterday", -1 }, { "i går", -1 }, { "igår", -1 },
};

/* Hinnant's days_from_civil. */
int
day_from_civil(Civil c)
{
	int y = c.m <= 2 ? c.y - 1 : c.y;
	int era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - era * 400;
	int doy = (153 * (c.m + (c.m > 2 ? -3 : 9)) + 2) / 5 + c.d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / CENTURY + doy;

	return era * DAYS_400Y + doe - DAYS_TO_1970;
}

Civil
civil_from_day(int day)
{
	int z = day + DAYS_TO_1970;
	int era = (z >= 0 ? z : z - DAYS_400Y + 1) / DAYS_400Y;
	int doe = z - era * DAYS_400Y;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / (DAYS_400Y - 1)) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / CENTURY);
	int mp = (5 * doy + 2) / 153;
	Civil c;

	c.d = doy - (153 * mp + 2) / 5 + 1;
	c.m = mp < 10 ? mp + 3 : mp - 9;
	c.y = yoe + era * 400 + (c.m <= 2);
	return c;
}

/* Monday is 0. */
int
weekday(int day)
{
	return ((day + 3) % WEEK + WEEK) % WEEK;
}

const char *
weekday_name(int day)
{
	return weekday_names[weekday(day)];
}

const char *
month_name(int m)
{
	return month_names[m - 1];
}

int
iso_week(int day)
{
	int thursday = day - weekday(day) + 3;
	Civil c = civil_from_day(thursday);
	int jan1 = day_from_civil((Civil){ c.y, 1, 1 });

	return (thursday - jan1) / WEEK + 1;
}

static int
days_in_month(int y, int m)
{
	static const int dim[MONTHS] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
		30, 31 };
	int leap = (y % 4 == 0 && y % CENTURY != 0) || y % 400 == 0;

	return m == 2 && leap ? 29 : dim[m - 1];
}

int64_t
day_start(int day)
{
	Civil c = civil_from_day(day);
	struct tm tm = { .tm_year = c.y - 1900, .tm_mon = c.m - 1,
		.tm_mday = c.d, .tm_isdst = -1 };

	return (int64_t)mktime(&tm);
}

int
day_of(int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	if (!localtime_r(&tt, &tm))
		return 0;
	return day_from_civil((Civil){ tm.tm_year + 1900, tm.tm_mon + 1,
			tm.tm_mday });
}

int
today(void)
{
	return day_of((int64_t)time(NULL));
}

static int
valid_civil(Civil c)
{
	return c.y >= YEAR_MIN && c.y <= YEAR_MAX && c.m >= 1 && c.m <= MONTHS &&
		c.d >= 1 && c.d <= days_in_month(c.y, c.m);
}

static int
relative_day(const char *s, int base)
{
	char *end;
	long n = strtol(s, &end, 10);
	long unit = *end == 'w' || *end == 'u' ? WEEK : 1;

	if (end[unit > 1] || n > SHIFT_MAX / unit || n < -SHIFT_MAX / unit)
		return INT_MIN;
	return base + (int)(n * unit);
}

static int
word_day(const char *s, int base)
{
	for (size_t i = 0; i < LENGTH(day_words); i++)
		if (!strcasecmp(s, day_words[i].word))
			return base + day_words[i].offset;
	return INT_MIN;
}

static int
iso_day(const char *s)
{
	Civil c;
	int n = 0;

	if (sscanf(s, "%4d-%2d-%2d%n", &c.y, &c.m, &c.d, &n) != 3 || s[n] ||
			c.y <= 31)
		return INT_MIN;
	return valid_civil(c) ? day_from_civil(c) : INT_MIN;
}

/* Day first, year optional. */
static int
dmy_day(const char *s, int base)
{
	Civil c = { civil_from_day(base).y, 0, 0 };
	char sep[2];
	size_t digits;
	int n = 0;

	if (sscanf(s, "%2d%1[./-]%2d%n", &c.d, sep, &c.m, &n) != 3)
		return INT_MIN;
	n += s[n] && strchr("./-", s[n]);
	digits = strspn(s + n, "0123456789");
	if (digits > YEAR_DIGITS || s[n + (int)digits])
		return INT_MIN;
	if (digits)
		c.y = atoi(s + n);
	c.y += c.y < TWO_DIGITS ? Y2K : 0;
	return valid_civil(c) ? day_from_civil(c) : INT_MIN;
}

static int
pick_day(const char *in, int base)
{
	int day;

	if (in[0] == '+' || in[0] == '-')
		return relative_day(in, base);
	day = word_day(in, base);
	if (day == INT_MIN)
		day = iso_day(in);
	return day == INT_MIN ? dmy_day(in, base) : day;
}

int
parse_date(const char *s, int base)
{
	char in[INPUT_MAX];
	size_t n;
	int day, y;

	s += strspn(s, " ");
	snprintf(in, sizeof(in), "%s", s);
	for (n = strlen(in); n > 0 && in[n - 1] == ' '; n--)
		in[n - 1] = '\0';
	day = in[0] ? pick_day(in, base) : INT_MIN;
	if (day == INT_MIN)
		return INT_MIN;
	y = civil_from_day(day).y;
	return y >= YEAR_MIN && y <= YEAR_MAX ? day : INT_MIN;
}

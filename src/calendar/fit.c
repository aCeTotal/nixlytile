/* Text fitted to pixels. */
#include "calendar/view.h"

#include <ctype.h>

#define CUTS_MAX 512
#define SCRATCH  1024
#define ELLIPSIS "…"

typedef struct {
	size_t at[CUTS_MAX];
	int n;
} Cuts;

static Cuts cuts;
static char scratch[SCRATCH];

static int
is_cont(char c)
{
	return (c & 0xC0) == 0x80;
}

/* Code point boundaries. */
static void
cuts_of(const char *s, size_t n)
{
	cuts.n = 0;
	for (size_t i = 0; i <= n && cuts.n < CUTS_MAX; i++)
		if (i == n || !is_cont(s[i]))
			cuts.at[cuts.n++] = i;
}

static int
head_width(FitSpec f, size_t n)
{
	while (n > 0 && f.s[n - 1] == ' ')
		n--;
	snprintf(scratch, sizeof(scratch), "%.*s%s", (int)n, f.s, ELLIPSIS);
	return card_font_width(f.font, scratch);
}

static int
tail_fits(FitSpec f, size_t from, size_t len)
{
	if (strlen(f.s + from) + sizeof(ELLIPSIS) > len)
		return 0;
	snprintf(scratch, sizeof(scratch), "%s%s", ELLIPSIS, f.s + from);
	return card_font_width(f.font, scratch) <= f.max_w;
}

static int
whole_fits(FitSpec f, size_t len)
{
	return strlen(f.s) < len && card_font_width(f.font, f.s) <= f.max_w;
}

/* Ellipsis on the right. */
void
fit_text(char *dst, size_t len, FitSpec f)
{
	size_t n = MIN(strlen(f.s), len - sizeof(ELLIPSIS));
	int lo = 0, hi;

	if (whole_fits(f, len)) {
		snprintf(dst, len, "%s", f.s);
		return;
	}
	while (n > 0 && is_cont(f.s[n]))
		n--;
	cuts_of(f.s, n);
	hi = cuts.n - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;

		if (head_width(f, cuts.at[mid]) <= f.max_w)
			lo = mid;
		else
			hi = mid - 1;
	}
	head_width(f, cuts.at[lo]);
	snprintf(dst, len, "%s", scratch);
}

/* Left ellipsis keeps caret visible. */
void
fit_tail(char *dst, size_t len, FitSpec f)
{
	int lo = 1, hi;

	if (whole_fits(f, len)) {
		snprintf(dst, len, "%s", f.s);
		return;
	}
	cuts_of(f.s, strlen(f.s));
	hi = cuts.n - 1;
	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (tail_fits(f, cuts.at[mid], len))
			hi = mid;
		else
			lo = mid + 1;
	}
	tail_fits(f, cuts.at[lo], len);
	snprintf(dst, len, "%s", scratch);
}

char *
upcase(char *s)
{
	for (char *p = s; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	return s;
}

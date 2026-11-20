/* Item timing in words. */
#include "calendar/view.h"

#include <stdarg.h>

#define SOON_S (15 * MIN_S)
#define WORDS  48

static Phrase phrase(int tone, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static Phrase
phrase(int tone, const char *fmt, ...)
{
	Phrase p = { .tone = tone };
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(p.text, sizeof(p.text), fmt, ap);
	va_end(ap);
	return p;
}

/* Days, hours and minutes. */
static void
span_words(char *dst, size_t len, int64_t s)
{
	long long d = s / DAY_S, h = s % DAY_S / HOUR_S, m = s % HOUR_S / MIN_S;
	int n = 0;

	if (s < MIN_S) {
		snprintf(dst, len, "<1 min");
		return;
	}
	if (d)
		n += snprintf(dst + n, len - (size_t)n, "%lld d ", d);
	if (h)
		n += snprintf(dst + n, len - (size_t)n, "%lld h ", h);
	if (m)
		n += snprintf(dst + n, len - (size_t)n, "%lld min ", m);
	dst[n > 0 ? n - 1 : 0] = '\0';
}

/* Largest unit only. */
static void
ago_words(char *dst, size_t len, int64_t s)
{
	if (s >= DAY_S)
		snprintf(dst, len, "%lld d", (long long)(s / DAY_S));
	else if (s >= HOUR_S)
		snprintf(dst, len, "%lld h", (long long)(s / HOUR_S));
	else
		snprintf(dst, len, "%lld min", (long long)MAX(s / MIN_S, 1));
}

static void
clock_text(char *dst, size_t len, int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	localtime_r(&tt, &tm);
	strftime(dst, len, "%H:%M", &tm);
}

static void
date_text(char *dst, size_t len, int day)
{
	Civil c = civil_from_day(day);

	snprintf(dst, len, "%d %.3s", c.d, month_name(c.m));
}

static Phrase
timed_range(const CalItem *it, int last)
{
	char a[16], b[16];

	clock_text(a, sizeof(a), it->start);
	if (last > day_of(it->start)) {
		date_text(b, sizeof(b), last);
		return phrase(TONE_PLAIN, "%s → %s", a, b);
	}
	clock_text(b, sizeof(b), it->end);
	return phrase(TONE_PLAIN, "%s–%s", a, b);
}

Phrase
when_time(const CalItem *it)
{
	char a[16];
	int last = day_of(it->end - 1);

	if ((it->flags & CAL_TASK) && !(it->flags & CAL_ALLDAY) && it->start) {
		clock_text(a, sizeof(a), it->start);
		return phrase(TONE_PLAIN, "Due %s", a);
	}
	if (it->flags & CAL_TASK)
		return phrase(TONE_PLAIN, "%s", "");
	if (!(it->flags & CAL_ALLDAY))
		return timed_range(it, last);
	if (last == day_of(it->start))
		return phrase(TONE_PLAIN, "All day");
	date_text(a, sizeof(a), last);
	return phrase(TONE_PLAIN, "Until %s", a);
}

static Phrase
task_until(const CalItem *it, int64_t now)
{
	char s[WORDS];
	int days = day_of(it->start) - day_of(now);

	if (!it->start)
		return phrase(TONE_PAST, "No due date");
	if (!(it->flags & CAL_ALLDAY)) {
		span_words(s, sizeof(s), it->start > now ?
				it->start - now : now - it->start);
		return it->start > now ? phrase(TONE_PLAIN, "Due in %s", s) :
			phrase(TONE_ALERT, "Overdue · %s", s);
	}
	if (days == 0)
		return phrase(TONE_LIVE, "Due today");
	if (days == 1)
		return phrase(TONE_PLAIN, "Due tomorrow");
	return days > 0 ? phrase(TONE_PLAIN, "Due in %d d", days) :
		phrase(TONE_ALERT, "Overdue · %d d", -days);
}

static Phrase
allday_until(const CalItem *it, int64_t now)
{
	int days = day_of(it->start) - day_of(now);
	int left = day_of(it->end - 1) - day_of(now);

	if (it->start <= now && now < it->end)
		return left ? phrase(TONE_LIVE, "Now · ends in %d d", left) :
			phrase(TONE_LIVE, "Today");
	if (days == 1)
		return phrase(TONE_PLAIN, "Tomorrow");
	return days > 0 ? phrase(TONE_PLAIN, "in %d d", days) :
		phrase(TONE_PAST, "%d d ago", -left);
}

Phrase
when_until(const CalItem *it, int64_t now)
{
	char s[WORDS];

	if (it->flags & CAL_TASK)
		return task_until(it, now);
	if (it->flags & CAL_ALLDAY)
		return allday_until(it, now);
	if (it->start > now) {
		span_words(s, sizeof(s), it->start - now);
		return phrase(it->start - now <= SOON_S ? TONE_LIVE : TONE_PLAIN,
				"in %s", s);
	}
	if (now < it->end) {
		span_words(s, sizeof(s), it->end - now);
		return phrase(TONE_LIVE, "Now · ends in %s", s);
	}
	ago_words(s, sizeof(s), now - it->end);
	return phrase(TONE_PAST, "%s ago", s);
}

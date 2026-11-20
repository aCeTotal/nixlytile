/* Untrusted iCalendar parsing within limits. */
#include <string.h>
#include <strings.h>

#include "ics.h"

#define NEST_MAX       8
#define LINES_MAX      1000000
#define BEGIN_TAG      "BEGIN"
#define END_TAG        "END"
#define HEAD_MAX       (sizeof BEGIN_TAG)
#define UTF8_BOM       "\xef\xbb\xbf"

/* Lines split as libical splits. */
typedef struct {
	const char *p;
	const char *last_lf;
} Scan;

/* Past last LF, CR splits. */
static size_t
physical_len(const Scan *s)
{
	if (s->last_lf && s->p <= s->last_lf)
		return (size_t)(strchr(s->p, '\n') - s->p);
	return strcspn(s->p, "\r");
}

/* Trailing CRs after appending bytes. */
static size_t
trailing_crs(const char *p, size_t len, size_t before)
{
	size_t n = 0;

	while (n < len && p[len - 1 - n] == '\r')
		n++;
	return n == len ? before + n : n;
}

/* Start of next unfolded line. */
static void
next_line(Scan *s, char *head)
{
	size_t n = 0, kept = 0, crs = 0, len, copied;
	const char *end;

	for (;;) {
		len = physical_len(s);
		end = s->p + len;
		copied = len < HEAD_MAX - n ? len : HEAD_MAX - n;
		memcpy(head + n, s->p, copied);
		n += copied;
		kept += len;
		crs = trailing_crs(s->p, len, crs);
		if (!*end || !kept || (end[1] != ' ' && end[1] != '\t'))
			break;
		if (crs) {
			n -= kept <= HEAD_MAX;
			kept--;
			crs--;
		}
		s->p = end + 2;
	}
	s->p = *end ? end + 1 : end;
	head[n] = '\0';
}

/* Tag, then colon or semicolon. */
static int
tagged(const char *head, const char *tag)
{
	size_t n = strlen(tag);

	return !strncasecmp(head, tag, n) && (head[n] == ':' || head[n] == ';');
}

/* Deep, huge trees break libical. */
static int
too_complex(const char *text)
{
	Scan s = { text, strrchr(text, '\n') };
	char head[HEAD_MAX + 1];
	int depth = 0;
	long lines = 0;

	if (!strncmp(s.p, UTF8_BOM, sizeof UTF8_BOM - 1))
		s.p += sizeof UTF8_BOM - 1;
	while (*s.p) {
		next_line(&s, head);
		depth += tagged(head, BEGIN_TAG) - (depth > 0 && tagged(head, END_TAG));
		if (depth > NEST_MAX || ++lines > LINES_MAX)
			return 1;
	}
	return 0;
}

icalcomponent *
ics_parse(const char *text)
{
	return too_complex(text) ? NULL : icalparser_parse_string(text);
}

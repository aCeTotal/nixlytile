/* Single-line text entry for popups. */
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "netsys.h"

#define TE_MAX TEXT_ENTRY_MAX

static char te_buf[TE_MAX];
static char te_disp[TE_MAX * 3];
static char te_label[64];
static int te_on;
static int te_masked;
static text_entry_submit_fn te_submit;
static void *te_data;
static unsigned te_gen;

static int
printable(unsigned char c)
{
	return c >= 0x20 && c != 0x7f;
}

/* Valid sequence length, else 0. */
static size_t
utf8_len(const char *s)
{
	unsigned char c = (unsigned char)*s;
	size_t n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;

	if ((c >= 0x80 && c < 0xc0) || c >= 0xf8)
		return 0;
	for (size_t k = 1; k < n; k++)
		if (((unsigned char)s[k] & 0xc0) != 0x80)
			return 0;
	return n == 1 && !printable(c) ? 0 : n;
}

void
text_entry_begin(const char *label, int masked,
		text_entry_submit_fn submit, void *data)
{
	snprintf(te_label, sizeof(te_label), "%s", label);
	te_buf[0] = '\0';
	te_on = 1;
	te_masked = masked;
	te_submit = submit;
	te_data = data;
	te_gen++;
	netsys_changed();
}

void
text_entry_cancel(void)
{
	te_on = 0;
	te_gen++;
	te_submit = NULL;
	memset(te_buf, 0, sizeof(te_buf));
	netsys_changed();
}

int
text_entry_active(void)
{
	return te_on;
}

text_entry_submit_fn
text_entry_owner(void)
{
	return te_on ? te_submit : NULL;
}

unsigned
text_entry_generation(void)
{
	return te_gen;
}

const char *
text_entry_label(void)
{
	return te_label;
}

const char *
text_entry_text(void)
{
	return te_buf;
}

void
text_entry_set_text(const char *text)
{
	snprintf(te_buf, sizeof(te_buf), "%s", text ? text : "");
	netsys_changed();
}

const char *
text_entry_display(void)
{
	if (!te_masked) {
		snprintf(te_disp, sizeof(te_disp), "%s_", te_buf);
	} else {
		size_t i, n = strlen(te_buf);

		te_disp[0] = '\0';
		for (i = 0; i < n && i < TE_MAX - 1; i++)
			strcat(te_disp, "•");
		strcat(te_disp, "_");
	}
	return te_disp;
}

int
text_entry_key(uint32_t keysym, const char *utf8)
{
	size_t len;

	if (!te_on)
		return 0;
	switch (keysym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter: {
		text_entry_submit_fn fn = te_submit;
		void *data = te_data;
		char text[TE_MAX];

		snprintf(text, sizeof(text), "%s", te_buf);
		te_on = 0;
		te_gen++;
		te_submit = NULL;
		memset(te_buf, 0, sizeof(te_buf));
		if (fn)
			fn(text, data);
		memset(text, 0, sizeof(text));
		netsys_changed();
		return 1;
	}
	case XKB_KEY_Escape:
		text_entry_cancel();
		return 1;
	case XKB_KEY_BackSpace:
		len = strlen(te_buf);
		/* strip one UTF-8 sequence, not one byte */
		while (len > 0 && (te_buf[len - 1] & 0xc0) == 0x80)
			len--;
		if (len > 0)
			len--;
		te_buf[len] = '\0';
		netsys_changed();
		return 1;
	default:
		break;
	}
	if (utf8 && utf8[0] && printable((unsigned char)utf8[0])) {
		len = strlen(te_buf);
		if (len + strlen(utf8) < sizeof(te_buf) - 1)
			strcat(te_buf, utf8);
		netsys_changed();
		return 1;
	}
	return 1;   /* swallow everything else while entry is active */
}

void
text_entry_insert(const char *s)
{
	size_t len = strlen(te_buf);

	while (*s && *s != '\n' && *s != '\r') {
		size_t n = utf8_len(s);

		if (!n) {
			s++;
			continue;
		}
		if (len + n > TE_MAX - 1)
			break;
		memcpy(te_buf + len, s, n);
		len += n;
		s += n;
	}
	while (len > 0 && te_buf[len - 1] == ' ')
		len--;
	te_buf[len] = '\0';
	netsys_changed();
}

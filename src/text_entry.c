/* Minimal single-line text entry for statusbar popups (SSID and
 * passphrase input).  While active, keypress() feeds keys here instead
 * of clients; Enter submits, Escape cancels.  Masked mode renders dots
 * so passphrases never hit the screen.
 */
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "netsys.h"

#define TE_MAX 128

static char te_buf[TE_MAX];
static char te_disp[TE_MAX * 3];
static char te_label[64];
static int te_on;
static int te_masked;
static text_entry_submit_fn te_submit;
static void *te_data;

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
	netsys_changed();
}

void
text_entry_cancel(void)
{
	te_on = 0;
	te_submit = NULL;
	memset(te_buf, 0, sizeof(te_buf));
	netsys_changed();
}

int
text_entry_active(void)
{
	return te_on;
}

const char *
text_entry_label(void)
{
	return te_label;
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
	if (utf8 && utf8[0] && (unsigned char)utf8[0] >= 0x20 &&
			utf8[0] != 0x7f) {
		len = strlen(te_buf);
		if (len + strlen(utf8) < sizeof(te_buf) - 1)
			strcat(te_buf, utf8);
		netsys_changed();
		return 1;
	}
	return 1;   /* swallow everything else while entry is active */
}

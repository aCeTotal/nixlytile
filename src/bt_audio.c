/* When a bluetooth audio device connects, force its PipeWire card onto
 * the best A2DP profile instead of whatever it came up on (a mic grab
 * at connect time can leave it stuck on HFP = telephone-quality sound).
 * Codec rank favours low latency first, then quality: aptX-LL, aptX-HD,
 * LDAC, AAC, aptX, SBC-XQ, plain a2dp, SBC.
 *
 * The card takes a moment to appear after Connect, so the check runs on
 * a timer and retries a few times.  All process I/O goes through
 * fetch_async (posix_spawn) — nothing blocks the compositor.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#include "netsys.h"
#include "fetch_async.h"

extern struct wl_event_loop *event_loop;

static struct wl_event_source *ba_timer;
static char ba_card[64];        /* bluez_card.AA_BB_.. we are fixing */
static int ba_tries;

static const char *rank[] = {
	"a2dp-sink-aptx_ll", "a2dp-sink-aptx_hd", "a2dp-sink-ldac",
	"a2dp-sink-aac", "a2dp-sink-aptx", "a2dp-sink-sbc_xq",
	"a2dp-sink", "a2dp-sink-sbc",
};

static int
profile_rank(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(rank) / sizeof(rank[0]); i++)
		if (strcmp(name, rank[i]) == 0)
			return (int)i;
	return -1;
}

static void ba_schedule(int delay_ms);

/* pw-dump of one device object: pick pw id, the EnumProfile entries and
 * the current Profile, then set the best-ranked a2dp profile if the
 * card is not already on it. */
static void
ba_dump_done(const char *out, size_t len, void *data)
{
	const char *p;
	char cur[64] = "";
	int pwid = -1;
	int best = -1, best_idx = -1;
	int in_enum = 0;

	if (!out[0]) {
		/* card not up yet */
		if (++ba_tries < 6)
			ba_schedule(2000);
		return;
	}
	p = strstr(out, "\"id\":");
	if (p)
		pwid = atoi(p + 5);

	/* walk "index"/"name" pairs; entries after "EnumProfile" are the
	 * choices, the pair after "Profile" is the active one */
	for (p = out; (p = strchr(p, '"')) != NULL; p++) {
		if (strncmp(p, "\"EnumProfile\"", 13) == 0) {
			in_enum = 1;
		} else if (strncmp(p, "\"Profile\"", 9) == 0) {
			in_enum = 0;
		} else if (strncmp(p, "\"index\":", 8) == 0) {
			const char *nm = strstr(p, "\"name\":");
			char name[64];
			int idx = atoi(p + 8), i = 0;

			if (!nm)
				break;
			nm = strchr(nm + 7, '"');
			if (!nm)
				break;
			nm++;
			while (*nm && *nm != '"' && i < (int)sizeof(name) - 1)
				name[i++] = *nm++;
			name[i] = '\0';
			if (in_enum) {
				int r = profile_rank(name);

				if (r >= 0 && (best < 0 || r < best)) {
					best = r;
					best_idx = idx;
				}
			} else {
				snprintf(cur, sizeof(cur), "%s", name);
			}
			p = nm;
		}
	}

	if (pwid < 0 || best_idx < 0)
		return;
	if (cur[0] && profile_rank(cur) >= 0 && profile_rank(cur) <= best)
		return;         /* already on the best profile */
	{
		char cmd[128];

		snprintf(cmd, sizeof(cmd), "wpctl set-profile %d %d",
				pwid, best_idx);
		fetch_async(cmd, NULL, NULL);
	}
}

static int
ba_tick(void *data)
{
	char cmd[512];

	/* find the pw device whose device.name carries our card id and
	 * dump just that object (keeps the output far under the
	 * fetch_async cap) */
	snprintf(cmd, sizeof(cmd),
			"for id in $(wpctl status | sed -n '/Devices:/,/Sinks:/p' "
			"| grep -oE ' [0-9]+\\.' | tr -dc '0-9\\n'); do "
			"wpctl inspect $id 2>/dev/null | grep -q '%s' && "
			"{ pw-dump $id; break; }; done", ba_card);
	if (fetch_async(cmd, ba_dump_done, NULL) != 0 && ++ba_tries < 6)
		ba_schedule(2000);
	return 0;
}

static void
ba_schedule(int delay_ms)
{
	if (!ba_timer)
		ba_timer = wl_event_loop_add_timer(event_loop, ba_tick, NULL);
	if (ba_timer)
		wl_event_source_timer_update(ba_timer, delay_ms);
}

void
bt_audio_on_connect(const char *addr, const char *icon)
{
	int i;

	if (!icon || strncmp(icon, "audio", 5) != 0)
		return;
	snprintf(ba_card, sizeof(ba_card), "bluez_card.%s", addr);
	for (i = 0; ba_card[i]; i++)
		if (ba_card[i] == ':')
			ba_card[i] = '_';
	ba_tries = 0;
	ba_schedule(2500);
}

/* PipeWire sink/source enumeration and default-device switching for the
 * volume/mic popups, via wpctl. */
#include "nixlytile.h"

/* Parse "wpctl status": devices live under "Sinks:"/"Sources:" as
 *  " │  *   72. MOMENTUM 4    [vol: 0.70]"  (star = current default).
 * Returns number of devices written to out. */
int
audio_list_devices(int sources, AudioDevice *out, int max)
{
	FILE *fp;
	char line[256];
	int in_section = 0, count = 0;

	if (!out || max <= 0)
		return 0;

	fp = popen("wpctl status", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp) && count < max) {
		char *p, *dot, *end;
		AudioDevice *d;
		unsigned long id;
		int is_default;

		if (strstr(line, sources ? "Sources:" : "Sinks:")) {
			in_section = 1;
			continue;
		}
		if (!in_section)
			continue;
		if (strstr(line, "Sinks:") || strstr(line, "Sources:") ||
				strstr(line, "Filters:") || strstr(line, "Streams:") ||
				!strncmp(line, "Video", 5))
			break;

		is_default = strchr(line, '*') != NULL;

		/* "  72. Name ..." — find the id number before ". " */
		p = line;
		while (*p && !(*p >= '0' && *p <= '9'))
			p++;
		if (!*p)
			continue;
		id = strtoul(p, &dot, 10);
		if (dot == p || dot[0] != '.' || dot[1] != ' ')
			continue;

		d = &out[count];
		d->id = (uint32_t)id;
		d->is_default = is_default;

		p = dot + 2;
		while (*p == ' ')
			p++;
		snprintf(d->name, sizeof(d->name), "%s", p);
		/* strip trailing "[vol: ...]" and whitespace */
		end = strrchr(d->name, '[');
		if (end)
			*end = '\0';
		end = d->name + strlen(d->name);
		while (end > d->name && (end[-1] == ' ' || end[-1] == '\n'))
			*--end = '\0';
		if (!d->name[0])
			continue;
		count++;
	}

	pclose(fp);
	return count;
}

void
audio_set_default(uint32_t id)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "wpctl set-default %u", id);
	run_wpctl_sync(cmd);
}

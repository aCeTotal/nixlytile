/* PipeWire sink/source enumeration and default-device switching for the
 * volume/mic popups, via wpctl. */
#include "nixlytile.h"

/* Parse one "  *   72. Name ...  [tag]" line into out; returns 0 on
 * success. Strips the trailing "[...]" block and whitespace. */
static int
parse_device_line(const char *line, AudioDevice *d)
{
	const char *p = line;
	char *dot, *end;
	unsigned long id;

	while (*p && !(*p >= '0' && *p <= '9'))
		p++;
	if (!*p)
		return -1;
	id = strtoul(p, &dot, 10);
	if (dot == (char *)p || dot[0] != '.' || dot[1] != ' ')
		return -1;

	d->id = (uint32_t)id;
	d->is_default = strchr(line, '*') != NULL;
	d->is_headset = 0;

	p = dot + 2;
	while (*p == ' ')
		p++;
	snprintf(d->name, sizeof(d->name), "%s", p);
	end = strrchr(d->name, '[');
	if (end)
		*end = '\0';
	end = d->name + strlen(d->name);
	while (end > d->name && (end[-1] == ' ' || end[-1] == '\n'))
		*--end = '\0';
	return d->name[0] ? 0 : -1;
}

/* Parse "wpctl status": devices live under "Sinks:"/"Sources:" as
 *  " │  *   72. MOMENTUM 4    [vol: 0.70]"  (star = current default).
 * Bluetooth headset mics are NOT listed under Sources: — WirePlumber
 * exposes them as loopback filters, so they appear under "Filters:"
 * tagged "[Audio/Source]" (and headset sinks "[Audio/Sink]").  Those
 * are included too, renamed after the bluez5 device from "Devices:".
 * Returns number of devices written to out. */
int
audio_list_devices(int sources, AudioDevice *out, int max)
{
	FILE *fp;
	char line[256];
	char bt_name[64] = "";
	const char *target = sources ? "Sources:" : "Sinks:";
	const char *filter_tag = sources ? "[Audio/Source]" : "[Audio/Sink]";
	int in_devices = 0, in_target = 0, in_filters = 0, count = 0;

	if (!out || max <= 0)
		return 0;

	fp = popen("wpctl status", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp) && count < max) {
		AudioDevice *d;

		/* Audio block only */
		if (!strncmp(line, "Video", 5) || !strncmp(line, "Settings", 8))
			break;

		if (strstr(line, "Devices:")) {
			in_devices = 1;
			in_target = in_filters = 0;
			continue;
		}
		if (strstr(line, target)) {
			in_target = 1;
			in_devices = in_filters = 0;
			continue;
		}
		if (strstr(line, "Filters:")) {
			in_filters = 1;
			in_devices = in_target = 0;
			continue;
		}
		if (strstr(line, "Sinks:") || strstr(line, "Sources:") ||
				strstr(line, "Streams:")) {
			in_devices = in_target = in_filters = 0;
			continue;
		}

		if (in_devices) {
			/* remember the bluetooth device's human name for the
			 * filter nodes below */
			AudioDevice dev;

			if (strstr(line, "[bluez5]") &&
					parse_device_line(line, &dev) == 0)
				snprintf(bt_name, sizeof(bt_name), "%s", dev.name);
			continue;
		}
		if (!in_target && !in_filters)
			continue;
		if (in_filters && !strstr(line, filter_tag))
			continue;

		d = &out[count];
		if (parse_device_line(line, d) != 0)
			continue;
		if (!strncmp(d->name, "bluez_input.", 12) ||
				!strncmp(d->name, "bluez_output.", 13)) {
			d->is_headset = 1;
			if (sources)
				snprintf(d->name, sizeof(d->name), "%s Mic",
						bt_name[0] ? bt_name : "Headset");
			else if (bt_name[0])
				snprintf(d->name, sizeof(d->name), "%s", bt_name);
			else
				snprintf(d->name, sizeof(d->name), "Headset");
		}
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

/* Headset plugged in / made the default sink: also route the default
 * mic to the headset's own microphone if it isn't already. */
void
audio_autoselect_headset_mic(void)
{
	AudioDevice devs[8];
	int n, def = -1, hs = -1;

	if (pipewire_sink_is_headset() != 1)
		return;
	n = audio_list_devices(1, devs, 8);
	for (int i = 0; i < n; i++) {
		if (devs[i].is_default)
			def = i;
		if (devs[i].is_headset && hs < 0)
			hs = i;
	}
	if (hs >= 0 && hs != def) {
		audio_set_default(devs[hs].id);
		mic_last_read_ms = 0;
		refreshstatusmic();
	}
}

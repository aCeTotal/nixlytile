#include "nixlytile.h"

/* SVG asset name to Nerd Font glyph. */
static const IconGlyph icon_glyphs[] = {
	{ "audio_dp.svg",            0xF0379, TONE_ACCENT },
	{ "audio_hdmi.svg",          0xF0841, TONE_ACCENT },
	{ "audio_jack.svg",          0xF02CB, TONE_ACCENT },
	{ "audio_mic.svg",           0xF036C, TONE_ACCENT },
	{ "audio_usb.svg",           0xF0553, TONE_ACCENT },
	{ "battery-25.svg",          0xF007C, TONE_ACCENT },
	{ "battery-50.svg",          0xF007E, TONE_ACCENT },
	{ "battery-75.svg",          0xF0080, TONE_ACCENT },
	{ "battery-100.svg",         0xF0079, TONE_ACCENT },
	{ "bluetooth.svg",           0xF00AF, TONE_ACCENT },
	{ "bluetooth_disabled.svg",  0xF00B2, TONE_CRIT },
	{ "bluetooth_searching.svg", 0xF00AF, TONE_ACCENT },
	{ "bt_batt_25.svg",          0xF007C, TONE_ACCENT },
	{ "bt_batt_50.svg",          0xF007E, TONE_ACCENT },
	{ "bt_batt_75.svg",          0xF0080, TONE_ACCENT },
	{ "bt_batt_100.svg",         0xF0079, TONE_ACCENT },
	{ "bt_computer.svg",         0xF0322, TONE_ACCENT },
	{ "bt_controller.svg",       0xF0EB5, TONE_ACCENT },
	{ "bt_display.svg",          0xF0379, TONE_ACCENT },
	{ "bt_headset.svg",          0xF02CE, TONE_ACCENT },
	{ "bt_keyboard.svg",         0xF030C, TONE_ACCENT },
	{ "bt_mic_warn.svg",         0xF0026, TONE_WARN },
	{ "bt_mouse.svg",            0xF037D, TONE_ACCENT },
	{ "bt_phone.svg",            0xF03F2, TONE_ACCENT },
	{ "bt_speaker.svg",          0xF04C3, TONE_ACCENT },
	{ "bt_watch.svg",            0xF0897, TONE_ACCENT },
	{ "clock.svg",               GLYPH_CLOCK, TONE_ACCENT },
	{ "cpu.svg",                 GLYPH_CPU, TONE_ACCENT },
	{ "disk.svg",                GLYPH_DISK, TONE_ACCENT },
	{ "ethernet.svg",            0xF0200, TONE_ACCENT },
	{ "headset.svg",             0xF02CB, TONE_ACCENT },
	{ "headset_muted.svg",       0xF07CE, TONE_CRIT },
	{ "light.svg",               GLYPH_LIGHT, TONE_ACCENT },
	{ "lock_closed.svg",         0xF033E, TONE_ACCENT },
	{ "lock_open.svg",           0xF033F, TONE_ACCENT },
	{ "microphone.svg",          0xF036C, TONE_ACCENT },
	{ "microphone_muted.svg",    0xF036D, TONE_CRIT },
	{ "monitor.svg",             GLYPH_DISPLAY, TONE_ACCENT },
	{ "no_connection.svg",       GLYPH_NET_OFF, TONE_CRIT },
	{ "power_lock.svg",          0xF033E, TONE_ACCENT },
	{ "power_logout.svg",        0xF0343, TONE_ACCENT },
	{ "power_reboot.svg",        0xF0709, TONE_ACCENT },
	{ "power_shutdown.svg",      GLYPH_POWER, TONE_ACCENT },
	{ "ram.svg",                 GLYPH_RAM, TONE_ACCENT },
	{ "speaker_25.svg",          0xF057F, TONE_ACCENT },
	{ "speaker_50.svg",          0xF0580, TONE_ACCENT },
	{ "speaker_100.svg",         0xF057E, TONE_ACCENT },
	{ "speaker_muted.svg",       0xF075F, TONE_CRIT },
	{ "wifi_25.svg",             0xF091F, TONE_ACCENT },
	{ "wifi_50.svg",             0xF0922, TONE_ACCENT },
	{ "wifi_75.svg",             0xF0925, TONE_ACCENT },
	{ "wifi_100.svg",            0xF0928, TONE_ACCENT },
};

const IconGlyph *
icon_glyph_for(const char *path)
{
	const char *base = strrchr(path, '/');

	base = base ? base + 1 : path;
	for (size_t i = 0; i < LENGTH(icon_glyphs); i++)
		if (!strcmp(icon_glyphs[i].svg, base))
			return &icon_glyphs[i];
	return NULL;
}

uint32_t
icon_glyph_cp(const char *path)
{
	const IconGlyph *g = icon_glyph_for(path);

	return g ? g->cp : 0;
}

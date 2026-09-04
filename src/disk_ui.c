/*
 * disk_ui.c — disk statusbar module (icon left of the CPU module) and
 * its hover popup: internal disks with filesystem + free-space gauges,
 * USB drives with Open (file browser) and Format, and unformatted /
 * non-Linux disks with a guided Format view (partitions, filesystem,
 * name, erase) backed by the nixly-diskd helper via diskwatch's worker
 * thread.  Formatted internal disks are mounted immediately and written
 * into the nixlyos config (disks-auto.nix) so they persist.
 */
#include "nixlytile.h"
#include "popup_card.h"
#include "netsys.h"

/* Slider hit id — must match the id info_popup_hover() excludes from
 * the hover-pill highlight (popup_extra.c SLIDER_HIT_ID). */
#define DK_SLIDER_HIT    100

#define DK_HIT_BACK      400
#define DK_HIT_ERASE     401
#define DK_HIT_CREATE    402
#define DK_HIT_NAME      403
#define DK_HIT_FS_BASE   410   /* +0..5: ext4 btrfs xfs vfat exfat ntfs */
#define DK_HIT_FMT_BASE  420   /* + disk index: open the format view */
#define DK_HIT_OPEN_BASE 440   /* + flat usb part index: file browser */
#define DK_HIT_UFMT_BASE 470   /* + flat usb part index: format view */
#define DK_HIT_DEL_BASE  500   /* + partition index in the format view */

#define DK_MIN_PART_MIB  64

static const char *dk_fs_names[6] =
	{ "ext4", "btrfs", "xfs", "vfat", "exfat", "ntfs" };

static struct wlr_buffer *disk_icon_buf;
static int disk_icon_w, disk_icon_h;
static int disk_icon_loaded_h;
static char disk_icon_loaded_path[PATH_MAX];

static DiskSnapshot dsnap;

/* Format-view state — one at a time, popup-global like the net UI. */
static struct {
	int open;               /* 1 = format view instead of the list */
	char disk[40];
	int is_usb;
	double size_frac;       /* share of the free tail for the new part */
	int fs_idx;             /* index into dk_fs_names */
	char label[40];
	int editing;            /* text_entry is ours */
	int confirm;            /* 0 none, 1 erase armed, 2 create armed */
	int del_armed;          /* partition index armed for delete, -1 */
	char pending_open[40];  /* device waiting for its mount → xdg-open */
} dview = { .size_frac = 1.0, .del_armed = -1 };

/* ── bar icon ────────────────────────────────────────────────────── */

static void
drop_disk_icon_buffer(void)
{
	if (disk_icon_buf) {
		wlr_buffer_drop(disk_icon_buf);
		disk_icon_buf = NULL;
	}
	disk_icon_loaded_h = 0;
	disk_icon_w = disk_icon_h = 0;
	disk_icon_loaded_path[0] = '\0';
}

static int
ensure_disk_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = disk_icon_path;

	if (target_h <= 0)
		return -1;
	if (resolve_asset_path(disk_icon_path, resolved,
				sizeof(resolved)) == 0 && resolved[0])
		path = resolved;
	if (disk_icon_buf && disk_icon_loaded_h == target_h &&
			strncmp(disk_icon_loaded_path, path,
				sizeof(disk_icon_loaded_path)) == 0)
		return 0;
	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr)
				g_error_free(gerr);
			return -1;
		}
	}
	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;
	drop_disk_icon_buffer();
	disk_icon_buf = buf;
	disk_icon_w = w;
	disk_icon_h = h;
	disk_icon_loaded_h = target_h;
	snprintf(disk_icon_loaded_path, sizeof(disk_icon_loaded_path),
			"%s", path);
	return 0;
}

void
renderdisk(StatusModule *module, int bar_height, const char *text)
{
	(void)text;
	render_tray_icon_module(module, bar_height,
			ensure_disk_icon_buffer, &disk_icon_buf,
			&disk_icon_w, &disk_icon_h);
}

/* ── formatting helpers ──────────────────────────────────────────── */

static void
dk_human(unsigned long long b, char *out, size_t len)
{
	double v = (double)b;
	static const char *unit[] = { "B", "kB", "MB", "GB", "TB", "PB" };
	int u = 0;

	while (v >= 1000.0 && u < 5) {
		v /= 1000.0;
		u++;
	}
	if (v < 10.0 && u > 0)
		snprintf(out, len, "%.1f %s", v, unit[u]);
	else
		snprintf(out, len, "%.0f %s", v, unit[u]);
}

static int
dk_is_linux_fs(const char *fs)
{
	return strcmp(fs, "ext4") == 0 || strcmp(fs, "ext3") == 0 ||
		strcmp(fs, "ext2") == 0 || strcmp(fs, "btrfs") == 0 ||
		strcmp(fs, "xfs") == 0 || strcmp(fs, "f2fs") == 0;
}

static const char *
dk_part_name(const DiskPart *p)
{
	if (p->label[0])
		return p->label;
	return strrchr(p->dev, '/') ? strrchr(p->dev, '/') + 1 : p->dev;
}

static DiskDev *
dk_find_disk(const char *dev)
{
	for (int i = 0; i < dsnap.ndisks; i++)
		if (strcmp(dsnap.disks[i].dev, dev) == 0)
			return &dsnap.disks[i];
	return NULL;
}

/* Whole-device pseudo partition (superfloppy) — no real table. */
static int
dk_has_table(const DiskDev *d)
{
	return d->npart > 0 && strcmp(d->parts[0].dev, d->dev) != 0;
}

/* Free tail after the last partition, in bytes. */
static unsigned long long
dk_free_tail(const DiskDev *d)
{
	unsigned long long end = 1024 * 1024;   /* 1 MiB alignment */

	if (!dk_has_table(d))
		return d->size_b > 2 * 1024 * 1024 ?
			d->size_b - 2 * 1024 * 1024 : 0;
	for (int i = 0; i < d->npart; i++) {
		unsigned long long e = d->parts[i].start_b +
			d->parts[i].size_b;

		if (e > end)
			end = e;
	}
	/* GPT keeps a backup table at the very end */
	if (d->size_b < end + 2 * 1024 * 1024)
		return 0;
	return d->size_b - end - 1024 * 1024;
}

/* A disk is "usable" when at least one partition is mounted with a
 * Linux filesystem — anything else lands in the bottom section. */
static int
dk_disk_usable(const DiskDev *d)
{
	for (int i = 0; i < d->npart; i++)
		if (d->parts[i].mount[0] &&
				dk_is_linux_fs(d->parts[i].fstype))
			return 1;
	return 0;
}

static const float *
dk_usage_color(double frac)
{
	return frac >= 0.9 ? card_col_red :
		frac >= 0.7 ? card_col_yellow : card_col_green;
}

/* ── popup: disk list ────────────────────────────────────────────── */

static void
dk_part_rows(Card *card, const DiskPart *p)
{
	char left[96], right[48], fr[24];
	double frac = p->size_b ? (double)p->used_b / p->size_b : 0.0;

	snprintf(left, sizeof(left), "%s  ·  %s", dk_part_name(p),
			p->fstype[0] ? p->fstype : "raw");
	dk_human(p->avail_b, fr, sizeof(fr));
	snprintf(right, sizeof(right), "%s free · %d%%", fr,
			(int)(frac * 100.0 + 0.5));
	card_text(card, left, right, dk_usage_color(frac));
	card_gauge(card, frac, dk_usage_color(frac));
	card_gap(card, 4);
}

static void
render_disk_list(Monitor *m, InfoPopup *p, Card *card)
{
	unsigned long long total_free = 0;
	char value[24], buf[96], sz[24];
	int shown;

	for (int d = 0; d < dsnap.ndisks; d++)
		for (int i = 0; i < dsnap.disks[d].npart; i++)
			if (dsnap.disks[d].parts[i].mount[0])
				total_free +=
					dsnap.disks[d].parts[i].avail_b;
	dk_human(total_free, sz, sizeof(sz));
	snprintf(value, sizeof(value), "%s", sz);
	card_header(card, disk_icon_path, "Storage", "DISKS FREE", value);
	card_gap(card, 6);

	if (dsnap.op_running) {
		card_loading(card, dsnap.op_msg[0] ? dsnap.op_msg :
				"Working on disk\xe2\x80\xa6",
				(double)(monotonic_msec() % 1000) / 1000.0);
		card_gap(card, 4);
	} else if (dsnap.op_msg[0]) {
		card_text(card, dsnap.op_msg, NULL,
				dsnap.op_failed ? card_col_red :
				card_col_green);
		card_gap(card, 4);
	}

	/* internal disks with a working Linux filesystem */
	shown = 0;
	for (int d = 0; d < dsnap.ndisks; d++) {
		DiskDev *dev = &dsnap.disks[d];

		if (dev->is_usb || !dk_disk_usable(dev))
			continue;
		if (!shown) {
			card_section(card, "INTERNAL");
			shown = 1;
		}
		dk_human(dev->size_b, sz, sizeof(sz));
		snprintf(buf, sizeof(buf), "%s", dev->model[0] ?
				dev->model : dev->dev);
		card_text(card, buf, sz, card_col_dim);
		for (int i = 0; i < dev->npart; i++)
			if (dev->parts[i].mount[0])
				dk_part_rows(card, &dev->parts[i]);
	}

	/* USB drives: every partition gets Open + Format */
	shown = 0;
	{
		int flat = -1;

		for (int d = 0; d < dsnap.ndisks; d++) {
			DiskDev *dev = &dsnap.disks[d];

			if (!dev->is_usb)
				continue;
			if (!shown) {
				card_section(card, "USB DRIVES");
				shown = 1;
			}
			dk_human(dev->size_b, sz, sizeof(sz));
			snprintf(buf, sizeof(buf), "%s", dev->model[0] ?
					dev->model : dev->dev);
			card_text(card, buf, sz, card_col_dim);
			if (dev->npart == 0) {
				flat++;
				card_text_btn2(card, "No filesystem",
						"Format",
						DK_HIT_UFMT_BASE + flat,
						p->btn_hover ==
						DK_HIT_UFMT_BASE + flat,
						NULL, -1, 0);
				continue;
			}
			for (int i = 0; i < dev->npart &&
					flat < DK_HIT_UFMT_BASE -
					DK_HIT_OPEN_BASE - 1; i++) {
				DiskPart *pt = &dev->parts[i];

				flat++;
				snprintf(buf, sizeof(buf), "%s  ·  %s",
						dk_part_name(pt),
						pt->fstype[0] ? pt->fstype :
						"raw");
				card_text_btn2(card, buf,
						"Open",
						DK_HIT_OPEN_BASE + flat,
						p->btn_hover ==
						DK_HIT_OPEN_BASE + flat,
						"Format",
						DK_HIT_UFMT_BASE + flat,
						p->btn_hover ==
						DK_HIT_UFMT_BASE + flat);
				if (pt->mount[0]) {
					double frac = pt->size_b ?
						(double)pt->used_b /
						pt->size_b : 0.0;

					card_gauge(card, frac,
							dk_usage_color(frac));
					card_gap(card, 4);
				}
			}
		}
	}

	/* not mounted / non-Linux internal disks, formattable */
	shown = 0;
	for (int d = 0; d < dsnap.ndisks; d++) {
		DiskDev *dev = &dsnap.disks[d];
		const char *state;

		if (dev->is_usb || dk_disk_usable(dev))
			continue;
		if (dev->is_system)
			continue;
		if (!shown) {
			card_section(card, "NOT IN USE");
			shown = 1;
		}
		state = "no filesystem";
		for (int i = 0; i < dev->npart; i++)
			if (dev->parts[i].fstype[0]) {
				state = dev->parts[i].fstype;
				break;
			}
		dk_human(dev->size_b, sz, sizeof(sz));
		snprintf(buf, sizeof(buf), "%s  ·  %s  ·  %s",
				dev->model[0] ? dev->model : dev->dev,
				sz, state);
		card_text_rbtn(card, buf, NULL, NULL, "Format disk",
				DK_HIT_FMT_BASE + d,
				p->btn_hover == DK_HIT_FMT_BASE + d);
	}

	if (!dsnap.helper_ok) {
		card_section(card, NULL);
		card_text(card, "Formatting needs nixly-diskd",
				"helper off", card_col_dim);
	}
	(void)m;
}

/* ── popup: format view ──────────────────────────────────────────── */

static void
render_format_view(Monitor *m, InfoPopup *p, Card *card)
{
	DiskDev *dev = dk_find_disk(dview.disk);
	unsigned long long tail, new_mib;
	char sz[24], buf[112], val[48];
	int table;

	(void)m;
	if (!dev) {   /* unplugged mid-edit */
		dview.open = 0;
		card_text(card, "Disk removed", NULL, card_col_red);
		return;
	}
	table = dk_has_table(dev);
	tail = dk_free_tail(dev);
	new_mib = (unsigned long long)((double)(tail / (1024 * 1024)) *
			dview.size_frac);

	dk_human(dev->size_b, sz, sizeof(sz));
	card_header(card, disk_icon_path, "Format disk",
			dev->model[0] ? dev->model : dview.disk, sz);
	card_gap(card, 2);
	card_icon_text(card, NULL, "\xe2\x80\xb9 Back to disks", NULL,
			DK_HIT_BACK, p->btn_hover == DK_HIT_BACK);

	if (dsnap.op_running) {
		card_loading(card, "Working on disk\xe2\x80\xa6",
				(double)(monotonic_msec() % 1000) / 1000.0);
		return;
	}

	card_section(card, "PARTITIONS");
	if (!table) {
		card_text(card, dev->npart ? "Whole-device filesystem" :
				"No partition table", NULL, card_col_dim);
	} else {
		for (int i = 0; i < dev->npart; i++) {
			DiskPart *pt = &dev->parts[i];
			const char *base = strrchr(pt->dev, '/');

			dk_human(pt->size_b, sz, sizeof(sz));
			snprintf(buf, sizeof(buf), "%s  ·  %s",
					base ? base + 1 : pt->dev,
					pt->fstype[0] ? pt->fstype : "raw");
			card_text_rbtn(card, buf, sz, card_col_dim,
					dview.del_armed == i ? "Confirm" :
					"Delete", DK_HIT_DEL_BASE + i,
					p->btn_hover == DK_HIT_DEL_BASE + i);
		}
	}

	card_section(card, "NEW PARTITION");
	if (tail / (1024 * 1024) < DK_MIN_PART_MIB && table) {
		card_text(card, "No free space \xe2\x80\x94 delete a "
				"partition or erase the disk", NULL,
				card_col_dim);
	} else {
		char szf[24];

		dk_human(new_mib * 1024ull * 1024ull, sz, sizeof(sz));
		dk_human(tail, szf, sizeof(szf));
		snprintf(val, sizeof(val), "%s of %s", sz, szf);
		card_kv2(card, "Size", val, NULL, NULL, NULL, NULL);
		card_gauge_id(card, dview.size_frac, card_col_blue,
				DK_SLIDER_HIT);
		card_gap(card, 6);
	}

	card_section(card, "FILESYSTEM");
	{
		int hov = p->btn_hover >= DK_HIT_FS_BASE &&
			p->btn_hover < DK_HIT_FS_BASE + 3 ?
			p->btn_hover - DK_HIT_FS_BASE : -1;

		card_buttons(card, dk_fs_names, NULL, 3,
				dview.fs_idx < 3 ? dview.fs_idx : -1,
				hov, DK_HIT_FS_BASE);
		card_gap(card, 4);
		hov = p->btn_hover >= DK_HIT_FS_BASE + 3 &&
			p->btn_hover < DK_HIT_FS_BASE + 6 ?
			p->btn_hover - DK_HIT_FS_BASE - 3 : -1;
		card_buttons(card, dk_fs_names + 3, NULL, 3,
				dview.fs_idx >= 3 ? dview.fs_idx - 3 : -1,
				hov, DK_HIT_FS_BASE + 3);
	}
	card_gap(card, 4);

	if (dview.editing && text_entry_active()) {
		snprintf(buf, sizeof(buf), "%s", text_entry_display());
		card_kv2(card, "Name", buf, card_col_blue, NULL, NULL, NULL);
		card_text(card, "Enter saves \xc2\xb7 Esc cancels", NULL,
				card_col_faint);
	} else {
		card_text_btn(card, "Name",
				dview.label[0] ? dview.label : "\xe2\x80\x94",
				NULL, "Edit", DK_HIT_NAME,
				p->btn_hover == DK_HIT_NAME);
	}
	card_gap(card, 6);

	if (tail / (1024 * 1024) >= DK_MIN_PART_MIB || !table)
		card_big_btn(card, dview.confirm == 2 ?
				"Confirm \xe2\x80\x94 create & format" :
				"Create partition & format",
				dview.confirm == 2 ? card_col_red :
				card_col_blue, DK_HIT_CREATE,
				p->btn_hover == DK_HIT_CREATE);
	card_gap(card, 4);
	card_big_btn(card, dview.confirm == 1 ?
			"Confirm \xe2\x80\x94 erase EVERYTHING" :
			"Erase entire disk & format",
			card_col_red, DK_HIT_ERASE,
			p->btn_hover == DK_HIT_ERASE);
	if (!dview.is_usb)
		card_text(card, "Mounts under /mnt and is added to the "
				"nixlyos config", NULL, card_col_faint);
	else
		card_text(card, "Mounts under /run/media", NULL,
				card_col_faint);
}

void
render_disk_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.disk_popup;
	Card *card;
	CardResult res;

	if (!p->tree)
		return;
	diskwatch_get(&dsnap);
	diskwatch_refresh();   /* fast cadence while the card is up */

	card = card_begin();
	if (!card)
		return;
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));

	if (dview.open)
		render_format_view(m, p, card);
	else
		render_disk_list(m, p, card);

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

/* ── clicks / jobs ───────────────────────────────────────────────── */

static void
dk_label_submitted(const char *text, void *data)
{
	(void)data;
	dview.editing = 0;
	snprintf(dview.label, sizeof(dview.label), "%s", text);
	/* strip characters the helper refuses */
	for (char *c = dview.label; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
			*c = '-';
	disk_popup_entry_changed();
}

/* flat usb-part index → device (matches the render order above) */
static DiskPart *
dk_usb_flat(int want, DiskDev **devout)
{
	int flat = -1;

	for (int d = 0; d < dsnap.ndisks; d++) {
		DiskDev *dev = &dsnap.disks[d];

		if (!dev->is_usb)
			continue;
		if (dev->npart == 0) {
			flat++;
			if (flat == want) {
				if (devout)
					*devout = dev;
				return NULL;   /* whole raw device */
			}
			continue;
		}
		for (int i = 0; i < dev->npart; i++) {
			flat++;
			if (flat == want) {
				if (devout)
					*devout = dev;
				return &dev->parts[i];
			}
		}
	}
	if (devout)
		*devout = NULL;
	return NULL;
}

static void
dk_open_format(const char *disk, int is_usb)
{
	dview.open = 1;
	snprintf(dview.disk, sizeof(dview.disk), "%s", disk);
	dview.is_usb = is_usb;
	dview.size_frac = 1.0;
	dview.fs_idx = is_usb ? 4 : 0;   /* exfat for sticks, ext4 inside */
	dview.label[0] = '\0';
	dview.confirm = 0;
	dview.del_armed = -1;
	if (dview.editing) {
		text_entry_cancel();
		dview.editing = 0;
	}
}

static void
dk_spawn_open(const char *dir)
{
	const char *const argv[] = { "xdg-open", dir, NULL };

	spawn_cmd_async(argv);
}

static void
dk_start_job(int erase)
{
	DiskDev *dev = dk_find_disk(dview.disk);
	DiskJob j = {0};
	unsigned long long tail_mib, start_mib;

	if (!dev)
		return;
	snprintf(j.disk, sizeof(j.disk), "%s", dview.disk);
	snprintf(j.fstype, sizeof(j.fstype), "%s",
			dk_fs_names[dview.fs_idx]);
	snprintf(j.label, sizeof(j.label), "%s", dview.label);
	j.format = 1;
	j.mount_after = 1;
	j.is_usb = dview.is_usb;

	if (erase || !dk_has_table(dev)) {
		j.wipe = 1;
		start_mib = 1;
		tail_mib = dev->size_b / (1024 * 1024);
		j.start_mib = start_mib;
		j.end_mib = tail_mib > 1 ? tail_mib - 1 : tail_mib;
		if (!erase && dk_has_table(dev))
			j.end_mib = j.start_mib +
				(unsigned long long)((double)(dk_free_tail(dev)
					/ (1024 * 1024)) * dview.size_frac);
	} else {
		unsigned long long end = 1024 * 1024;

		for (int i = 0; i < dev->npart; i++) {
			unsigned long long e = dev->parts[i].start_b +
				dev->parts[i].size_b;

			if (e > end)
				end = e;
		}
		start_mib = end / (1024 * 1024) + 1;
		j.start_mib = start_mib;
		j.end_mib = start_mib +
			(unsigned long long)((double)(dk_free_tail(dev) /
					(1024 * 1024)) * dview.size_frac);
		if (j.end_mib <= j.start_mib + 1)
			return;
	}
	diskwatch_run_job(&j);
	dview.open = 0;
	dview.confirm = 0;
}

int
disk_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.disk_popup;
	int popup_x, rel_x, rel_y;

	if (!p->visible || !p->tree || button != BTN_LEFT)
		return 0;

	popup_x = p->tree->node.x;
	rel_x = lx - popup_x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];
		int id;

		if (hit->w <= 0 ||
				rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		id = hit->id;

		if (id == DK_SLIDER_HIT) {
			double frac = (double)(rel_x - hit->x) / hit->w;

			if (frac < 0.05)
				frac = 0.05;
			if (frac > 1.0)
				frac = 1.0;
			dview.size_frac = frac;
			dview.confirm = 0;
			p->last_render_ms = 0;
			return 1;
		}
		if (id == DK_HIT_BACK) {
			dview.open = 0;
			if (dview.editing) {
				text_entry_cancel();
				dview.editing = 0;
			}
			p->last_render_ms = 0;
			return 1;
		}
		if (id >= DK_HIT_FS_BASE && id < DK_HIT_FS_BASE + 6) {
			dview.fs_idx = id - DK_HIT_FS_BASE;
			dview.confirm = 0;
			p->last_render_ms = 0;
			return 1;
		}
		if (id == DK_HIT_NAME) {
			dview.editing = 1;
			text_entry_begin("Volume name", 0,
					dk_label_submitted, NULL);
			if (dview.label[0])
				text_entry_set_text(dview.label);
			p->last_render_ms = 0;
			return 1;
		}
		if (id == DK_HIT_CREATE || id == DK_HIT_ERASE) {
			int want = id == DK_HIT_ERASE ? 1 : 2;

			if (!dsnap.helper_ok) {
				p->last_render_ms = 0;
				return 1;
			}
			if (dview.confirm == want)
				dk_start_job(want == 1);
			else
				dview.confirm = want;
			p->last_render_ms = 0;
			return 1;
		}
		if (id >= DK_HIT_DEL_BASE && id < DK_HIT_DEL_BASE + 32) {
			int idx = id - DK_HIT_DEL_BASE;
			DiskDev *dev = dk_find_disk(dview.disk);

			if (!dev || idx >= dev->npart || !dsnap.helper_ok)
				return 1;
			if (dview.del_armed == idx) {
				DiskJob j = {0};
				const char *base =
					strrchr(dev->parts[idx].dev, '/');
				size_t l;

				snprintf(j.disk, sizeof(j.disk), "%s",
						dview.disk);
				/* partition number = trailing digits */
				base = base ? base + 1 :
					dev->parts[idx].dev;
				l = strlen(base);
				while (l > 0 && isdigit(
						(unsigned char)base[l - 1]))
					l--;
				j.rmpart = atoi(base + l);
				dview.del_armed = -1;
				if (j.rmpart > 0)
					diskwatch_run_job(&j);
			} else {
				dview.del_armed = idx;
			}
			p->last_render_ms = 0;
			return 1;
		}
		if (id >= DK_HIT_FMT_BASE && id < DK_HIT_FMT_BASE + DISK_MAX) {
			int d = id - DK_HIT_FMT_BASE;

			if (d < dsnap.ndisks && !dsnap.disks[d].is_system) {
				dk_open_format(dsnap.disks[d].dev, 0);
				p->last_render_ms = 0;
			}
			return 1;
		}
		if (id >= DK_HIT_OPEN_BASE && id < DK_HIT_UFMT_BASE) {
			DiskDev *dev;
			DiskPart *pt = dk_usb_flat(id - DK_HIT_OPEN_BASE,
					&dev);

			if (pt && pt->mount[0]) {
				dk_spawn_open(pt->mount);
				info_popups_hide(m);
			} else if (pt && pt->fstype[0]) {
				DiskJob j = {0};

				/* mount first, open when it appears */
				snprintf(j.disk, sizeof(j.disk), "%s",
						dev ? dev->dev : "");
				snprintf(j.part, sizeof(j.part), "%s",
						pt->dev);
				snprintf(j.fstype, sizeof(j.fstype), "%s",
						pt->fstype);
				snprintf(j.label, sizeof(j.label), "%s",
						pt->label);
				j.mount_after = 1;
				j.is_usb = 1;
				snprintf(dview.pending_open,
						sizeof(dview.pending_open),
						"%s", pt->dev);
				diskwatch_run_job(&j);
				p->last_render_ms = 0;
			}
			return 1;
		}
		if (id >= DK_HIT_UFMT_BASE && id < DK_HIT_DEL_BASE) {
			DiskDev *dev;

			dk_usb_flat(id - DK_HIT_UFMT_BASE, &dev);
			if (dev) {
				dk_open_format(dev->dev, 1);
				p->last_render_ms = 0;
			}
			return 1;
		}
	}
	/* swallow clicks on the card body */
	return 1;
}

/* ── refresh plumbing ────────────────────────────────────────────── */

/* A text-entry keystroke landed while our Name field is editing —
 * re-render the open popup so the echo is immediate. */
void
disk_popup_entry_changed(void)
{
	Monitor *m;

	if (!dview.editing && text_entry_active())
		return;
	wl_list_for_each(m, &mons, link)
		if (m->statusbar.disk_popup.visible) {
			m->statusbar.disk_popup.last_render_ms = 0;
			schedule_popup_delay(1);
			return;
		}
}

/* Fresh snapshot published by diskwatch (or the periodic status task):
 * update the bar module and any open popup. */
void
refreshstatusdisk(void)
{
	Monitor *m;
	int barh;

	diskwatch_get(&dsnap);

	/* a mount we queued for "Open" just appeared → file browser */
	if (dview.pending_open[0]) {
		for (int d = 0; d < dsnap.ndisks; d++)
			for (int i = 0; i < dsnap.disks[d].npart; i++) {
				DiskPart *pt = &dsnap.disks[d].parts[i];

				if (strcmp(pt->dev, dview.pending_open) == 0
						&& pt->mount[0]) {
					dk_spawn_open(pt->mount);
					dview.pending_open[0] = '\0';
				}
			}
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.disk.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height :
			(int)statusbar_height;
		if (m->statusbar.disk.width <= 0) {
			renderdisk(&m->statusbar.disk, barh, NULL);
			positionstatusmodules(m);
		}
		if (m->statusbar.disk_popup.visible) {
			render_disk_popup(m);
			m->statusbar.disk_popup.last_render_ms =
				monotonic_msec();
		}
	}
}

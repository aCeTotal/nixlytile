#include "layer.h"

static int
live(struct vrs_link *page)
{
	return page && atomic_load(&page->active);
}

/* Active link settings, else native. */
static struct zones
zones_now(struct device *dev)
{
	struct vrs_link *page = link_page();
	struct zones z = {
		.display = extent_of(atomic_load(&dev->swap_extent)),
		.focus = VRS_FOCUS_NONE,
		.quality = VRS_QUALITY_MAX,
	};
	uint32_t quality;

	if (!live(page))
		return z;
	quality = atomic_load(&page->quality);
	z.focus = atomic_load(&page->focus);
	z.fovea = atomic_load(&page->fovea);
	z.blend = atomic_load(&page->blend);
	z.quality = quality < VRS_QUALITY_MAX ? quality : VRS_QUALITY_MAX;
	return z;
}

/* Steady state stays read-only. */
static int
consume(_Atomic int *flag)
{
	return atomic_load_explicit(flag, memory_order_relaxed) && atomic_exchange(flag, 0);
}

/* Live link, no split, uploadable. */
int
fovea_active(const struct device *dev)
{
	return !atomic_load(&dev->split) && !atomic_load(&dev->present_failed)
		&& live(link_page());
}

/* Wakes worker, uploads changed maps. */
void
fovea_present(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	struct zones now;
	int dirty;

	if (consume(&f->asked))
		worker_kick(&f->worker);
	if (atomic_load(&f->upload) != UPLOAD_READY || atomic_load(&dev->split))
		return;
	now = zones_now(dev);
	dirty = consume(&f->dirty);
	if (zones_equal(&now, &f->zones) && !dirty)
		return;
	f->zones = now;
	if (!upload_stale(dev))
		atomic_store(&f->dirty, 1);
}

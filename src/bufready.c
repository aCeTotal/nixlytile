/* Latch client buffers once rendered. */
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "nixlytile.h"

typedef struct {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
} Watch;

typedef struct {
	struct wlr_surface *surface;
	struct wl_event_source *source;
	struct wl_listener destroy;
	uint32_t seq;
	int fence;
} Hold;

static struct wl_listener new_surface;

static void
hold_free(Hold *h)
{
	wl_list_remove(&h->destroy.link);
	wl_event_source_remove(h->source);
	close(h->fence);
	free(h);
}

static int
hold_ready(int fd, uint32_t mask, void *data)
{
	Hold *h = data;

	wlr_surface_unlock_cached(h->surface, h->seq);
	hold_free(h);
	return 0;
}

static void
hold_surface_destroy(struct wl_listener *listener, void *data)
{
	Hold *h = wl_container_of(listener, h, destroy);

	hold_free(h);
}

/* Pending GPU writes, or -1. */
static int
write_fence(struct wlr_buffer *buffer)
{
	struct wlr_dmabuf_attributes dmabuf;
	struct dma_buf_export_sync_file sync = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
	struct pollfd p;

	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf))
		return -1;
	if (ioctl(dmabuf.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) < 0)
		return -1;
	p = (struct pollfd){ .fd = sync.fd, .events = POLLIN };
	if (poll(&p, 1, 0) == 0)
		return sync.fd;
	close(sync.fd);
	return -1;
}

static void
surface_commit(struct wl_listener *listener, void *data)
{
	Watch *w = wl_container_of(listener, w, commit);
	struct wlr_surface *surface = w->surface;
	Hold *h;
	int fence;

	if (!(surface->pending.committed & WLR_SURFACE_STATE_BUFFER) || !surface->pending.buffer)
		return;
	if (wlr_linux_drm_syncobj_v1_get_surface_state(surface))
		return;
	fence = write_fence(surface->pending.buffer);
	if (fence < 0)
		return;

	h = ecalloc(1, sizeof(*h));
	h->source = wl_event_loop_add_fd(event_loop, fence, WL_EVENT_READABLE, hold_ready, h);
	if (!h->source) {
		close(fence);
		free(h);
		return;
	}
	h->surface = surface;
	h->fence = fence;
	h->seq = wlr_surface_lock_pending(surface);
	h->destroy.notify = hold_surface_destroy;
	wl_signal_add(&surface->events.destroy, &h->destroy);
}

static void
surface_destroy(struct wl_listener *listener, void *data)
{
	Watch *w = wl_container_of(listener, w, destroy);

	wl_list_remove(&w->commit.link);
	wl_list_remove(&w->destroy.link);
	free(w);
}

static void
surface_new(struct wl_listener *listener, void *data)
{
	Watch *w = ecalloc(1, sizeof(*w));

	w->surface = data;
	w->commit.notify = surface_commit;
	wl_signal_add(&w->surface->events.client_commit, &w->commit);
	w->destroy.notify = surface_destroy;
	wl_signal_add(&w->surface->events.destroy, &w->destroy);
}

void
bufready_init(void)
{
	new_surface.notify = surface_new;
	wl_signal_add(&compositor->events.new_surface, &new_surface);
}

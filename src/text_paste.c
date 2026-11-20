/* Clipboard paste into entries. */
#include "nixlytile.h"
#include "netsys.h"

#include <errno.h>
#include <fcntl.h>

#define PASTE_MAX  2048
#define TIMEOUT_MS 2000

static const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
};

static struct {
	int fd;
	unsigned gen;
	struct wl_event_source *ev, *timer;
	char buf[PASTE_MAX];
	size_t len;
} paste = { .fd = -1 };

static void
paste_stop(void)
{
	if (paste.ev)
		wl_event_source_remove(paste.ev);
	if (paste.timer)
		wl_event_source_remove(paste.timer);
	if (paste.fd >= 0)
		close(paste.fd);
	explicit_bzero(paste.buf, sizeof(paste.buf));
	paste.ev = paste.timer = NULL;
	paste.fd = -1;
	paste.len = 0;
}

/* Only the entry that asked. */
static void
paste_end(void)
{
	paste.buf[paste.len] = '\0';
	if (text_entry_active() && text_entry_generation() == paste.gen)
		text_entry_insert(paste.buf);
	paste_stop();
}

static int
paste_readable(int fd, uint32_t mask, void *data)
{
	size_t room = sizeof(paste.buf) - 1 - paste.len;
	ssize_t n = 0;

	while (room && (n = read(fd, paste.buf + paste.len, room)) > 0) {
		paste.len += (size_t)n;
		room -= (size_t)n;
	}
	if (!room || n == 0 || (errno != EAGAIN && errno != EINTR))
		paste_end();
	return 0;
}

static int
paste_timeout(void *data)
{
	paste_stop();
	return 0;
}

static int
offered(struct nx_data_source *src, const char *mime)
{
	char **type;

	wl_array_for_each(type, &src->mime_types)
		if (!strcmp(*type, mime))
			return 1;
	return 0;
}

static const char *
text_mime(struct nx_data_source *src)
{
	for (size_t i = 0; i < LENGTH(text_mimes); i++)
		if (offered(src, text_mimes[i]))
			return text_mimes[i];
	return NULL;
}

void
text_entry_paste(void)
{
	struct nx_data_source *src = seat->selection_source;
	const char *mime = src ? text_mime(src) : NULL;
	int fds[2];

	paste_stop();
	if (!mime || pipe2(fds, O_CLOEXEC) < 0)
		return;
	fd_set_nonblock(fds[0]);
	nx_data_source_send(src, mime, fds[1]);
	paste.fd = fds[0];
	paste.gen = text_entry_generation();
	paste.ev = wl_event_loop_add_fd(event_loop, fds[0], WL_EVENT_READABLE,
			paste_readable, NULL);
	paste.timer = wl_event_loop_add_timer(event_loop, paste_timeout, NULL);
	if (!paste.ev || !paste.timer) {
		paste_stop();
		return;
	}
	wl_event_source_timer_update(paste.timer, TIMEOUT_MS);
}

#include <stdlib.h>
#include <string.h>
#include "platform/clipboard/mime_types.h"
#include "platform/log/log.h"

void mime_types_add(struct wl_array *mime_types, const char *mime_type,
		struct wl_resource *resource) {
	const char **p;
	wl_array_for_each(p, mime_types) {
		if (strcmp(*p, mime_type) == 0) {
			nx_log(NX_DEBUG, "Ignoring duplicate MIME type offer %s",
				mime_type);
			return;
		}
	}

	char *dup_mime_type = strdup(mime_type);
	if (dup_mime_type == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	char **slot = wl_array_add(mime_types, sizeof(*slot));
	if (slot == NULL) {
		free(dup_mime_type);
		wl_resource_post_no_memory(resource);
		return;
	}

	*slot = dup_mime_type;
}

void mime_types_release(struct wl_array *mime_types) {
	char **p;
	wl_array_for_each(p, mime_types) {
		free(*p);
	}
	wl_array_release(mime_types);
}

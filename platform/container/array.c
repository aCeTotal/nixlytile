#include <assert.h>
#include <string.h>
#include "platform/container/array.h"

static const size_t MIN_ALLOC = 16;

void array_remove_at(struct wl_array *arr, size_t offset, size_t size) {
	assert(arr->size >= offset + size);

	char *data = arr->data;
	memmove(&data[offset], &data[offset + size], arr->size - offset - size);
	arr->size -= size;
}

bool array_realloc(struct wl_array *arr, size_t size) {
	// Shrinks below a quarter for hysteresis.
	size_t alloc = MIN_ALLOC;
	if (arr->alloc > 0 && size > arr->alloc / 4) {
		alloc = arr->alloc;
	}
	while (alloc < size) {
		alloc *= 2;
	}

	if (alloc == arr->alloc) {
		return true;
	}

	void *data = realloc(arr->data, alloc);
	if (data == NULL) {
		return false;
	}
	arr->data = data;
	arr->alloc = alloc;
	return true;
}

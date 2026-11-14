#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/protocol/token.h"

static FILE *open_urandom(void) {
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open random device");
		return NULL;
	}
	FILE *urandom = fdopen(fd, "r");
	if (urandom == NULL) {
		nx_log_errno(NX_ERROR, "fdopen failed");
		close(fd);
	}
	return urandom;
}

bool generate_token(char out[static TOKEN_SIZE]) {
	static FILE *urandom = NULL;
	if (urandom == NULL && (urandom = open_urandom()) == NULL) {
		return false;
	}

	uint64_t data[2];
	if (fread(data, sizeof(data), 1, urandom) != 1) {
		nx_log_errno(NX_ERROR, "Failed to read from random device");
		return false;
	}
	snprintf(out, TOKEN_SIZE, "%016" PRIx64 "%016" PRIx64, data[0], data[1]);
	return true;
}

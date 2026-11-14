#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "platform/os/shm.h"

#define RANDNAME_PATTERN "/nixlytile-XXXXXX"

static const int RANDNAME_LEN = 6;
static const int OPEN_ATTEMPTS = 100;

static void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < RANDNAME_LEN; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

// shm_open always sets CLOEXEC.
static int excl_shm_open(char *name) {
	for (int i = 0; i < OPEN_ATTEMPTS; i++) {
		randname(name + strlen(RANDNAME_PATTERN) - RANDNAME_LEN);
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			return fd;
		}
		if (errno != EEXIST) {
			break;
		}
	}
	return -1;
}

static bool resize(int fd, size_t size) {
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	return ret == 0;
}

int allocate_shm_file(size_t size) {
	char name[] = RANDNAME_PATTERN;
	int fd = excl_shm_open(name);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);

	if (!resize(fd, size)) {
		close(fd);
		return -1;
	}
	return fd;
}

bool allocate_shm_file_pair(size_t size, int *rw_fd_ptr, int *ro_fd_ptr) {
	char name[] = RANDNAME_PATTERN;
	int rw_fd = excl_shm_open(name);
	if (rw_fd < 0) {
		return false;
	}

	int ro_fd = shm_open(name, O_RDONLY, 0);
	shm_unlink(name);
	if (ro_fd < 0) {
		close(rw_fd);
		return false;
	}

	// Blocks reopening via /proc/self/fd.
	if (fchmod(rw_fd, 0) != 0 || !resize(rw_fd, size)) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}

	*rw_fd_ptr = rw_fd;
	*ro_fd_ptr = ro_fd;
	return true;
}

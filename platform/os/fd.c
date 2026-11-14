#include <fcntl.h>
#include "platform/log/log.h"
#include "platform/os/fd.h"

bool set_cloexec(int fd, bool cloexec) {
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		nx_log_errno(NX_ERROR, "fcntl failed");
		return false;
	}
	flags = cloexec ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		nx_log_errno(NX_ERROR, "fcntl failed");
		return false;
	}
	return true;
}

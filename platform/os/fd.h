#ifndef PLATFORM_OS_FD_H
#define PLATFORM_OS_FD_H

#include <stdbool.h>

bool set_cloexec(int fd, bool cloexec);

#endif

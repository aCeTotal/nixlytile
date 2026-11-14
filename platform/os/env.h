#ifndef PLATFORM_OS_ENV_H
#define PLATFORM_OS_ENV_H

#include <stdbool.h>
#include <unistd.h>

// False unless set to 1.
bool env_parse_bool(const char *option);
// Index into NULL-terminated switches, else 0.
size_t env_parse_switch(const char *option, const char **switches);

#endif

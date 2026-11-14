#ifndef PLATFORM_PROTOCOL_UTF8_H
#define PLATFORM_PROTOCOL_UTF8_H

#include <stdbool.h>

// Well-formed per Unicode Table 3-7.
bool is_utf8(const char *string);

#endif

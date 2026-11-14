#ifndef PLATFORM_PROTOCOL_TOKEN_H
#define PLATFORM_PROTOCOL_TOKEN_H

#include <stdbool.h>

// 32 hex digits plus NUL.
#define TOKEN_SIZE 33

bool generate_token(char out[static TOKEN_SIZE]);

#endif

#ifndef PLATFORM_SESSION_SEAT_H
#define PLATFORM_SESSION_SEAT_H

#include <stdbool.h>

struct nx_session;

bool session_seat_init(struct nx_session *session);
void session_seat_finish(struct nx_session *session);

#endif

#ifndef PWAUDIBLE_H
#define PWAUDIBLE_H

#include <stdint.h>
#include <sys/types.h>

/* Async pw-dump; done runs after. */
void pwaudible_query(void (*done)(void));
int pwaudible_has(pid_t pid);
uint64_t pwaudible_at(void);

#endif

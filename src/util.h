/* See LICENSE.dwm file for copyright and license details. */

#include <stddef.h>
#include <sys/types.h>

void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
int fd_set_nonblock(int fd);
char *read_file_to_string(const char *path, size_t *out_len);
int spawn_async_read(const char *cmd, pid_t *out_pid, int *out_fd);

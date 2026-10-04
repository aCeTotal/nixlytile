#ifndef PROCSNAP_H
#define PROCSNAP_H

#include <sys/types.h>

#define PROCSNAP_MAX 4096

typedef struct {
	pid_t pid, ppid;
	int tty;
	unsigned long long start;
	char comm[16];
} Proc;

/* Sorted: /proc lists ascending. */
extern Proc procsnap[PROCSNAP_MAX];
extern int procsnap_n;

int procsnap_read(pid_t pid, Proc *p);
void procsnap_take(void);
int procsnap_find(pid_t pid);
int procsnap_in_tree(int idx, pid_t root);

#endif

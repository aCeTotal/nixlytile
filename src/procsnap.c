#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "procsnap.h"

#define PROCSNAP_DEPTH 64

Proc procsnap[PROCSNAP_MAX];
int procsnap_n;

int
procsnap_read(pid_t pid, Proc *p)
{
	char path[32], buf[512];
	char *open_paren, *close_paren;
	size_t n, clen;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	open_paren = strchr(buf, '(');
	close_paren = strrchr(buf, ')');
	if (!open_paren || !close_paren || close_paren < open_paren)
		return 0;
	clen = (size_t)(close_paren - open_paren - 1);
	if (clen >= sizeof(p->comm))
		clen = sizeof(p->comm) - 1;
	memcpy(p->comm, open_paren + 1, clen);
	p->comm[clen] = '\0';
	p->pid = pid;
	return sscanf(close_paren + 2,
		"%*c %d %*d %*d %d %*d %*u %*u %*u %*u %*u %*u %*u "
		"%*d %*d %*d %*d %*d %*d %llu",
		&p->ppid, &p->tty, &p->start) == 3;
}

void
procsnap_take(void)
{
	DIR *dir = opendir("/proc");
	uid_t uid = getuid();
	struct dirent *ent;
	struct stat st;

	procsnap_n = 0;
	if (!dir)
		return;
	while ((ent = readdir(dir)) && procsnap_n < PROCSNAP_MAX) {
		if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
			continue;
		if (fstatat(dirfd(dir), ent->d_name, &st, 0) != 0 || st.st_uid != uid)
			continue;
		if (procsnap_read((pid_t)atoi(ent->d_name), &procsnap[procsnap_n]))
			procsnap_n++;
	}
	closedir(dir);
}

int
procsnap_find(pid_t pid)
{
	int lo = 0, hi = procsnap_n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (procsnap[mid].pid == pid)
			return mid;
		if (procsnap[mid].pid < pid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

int
procsnap_in_tree(int idx, pid_t root)
{
	int depth;

	for (depth = 0; idx >= 0 && depth < PROCSNAP_DEPTH; depth++) {
		if (procsnap[idx].pid == root)
			return 1;
		idx = procsnap_find(procsnap[idx].ppid);
	}
	return 0;
}

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "profile.h"

static int
state_dir(char *dir, size_t len)
{
	const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	int n;

	if (state && *state)
		n = snprintf(dir, len, "%s/nixlyos", state);
	else if (home)
		n = snprintf(dir, len, "%s/.local/state/nixlyos", home);
	else
		return -1;
	return n > 0 && (size_t)n < len ? 0 : -1;
}

static int
profile_path(char *path, size_t len)
{
	char dir[PATH_MAX];

	if (state_dir(dir, sizeof(dir)) < 0)
		return -1;
	mkdir(dir, 0755);
	return snprintf(path, len, "%s/gaze", dir) < (int)len ? 0 : -1;
}

static void
write_row(FILE *fp, const char *key, const float *v, int n)
{
	fputs(key, fp);
	for (int i = 0; i < n; i++)
		fprintf(fp, " %.9g", (double)v[i]);
	fputc('\n', fp);
}

static int
read_row(FILE *fp, const char *key, float *v, int n)
{
	char word[32];

	if (fscanf(fp, "%31s", word) != 1 || strcmp(word, key))
		return -1;
	for (int i = 0; i < n; i++)
		if (fscanf(fp, "%f", &v[i]) != 1)
			return -1;
	return 0;
}

int
profile_save(const Profile *p, const Sample *s, int n)
{
	char path[PATH_MAX], tmp[PATH_MAX + 4];
	FILE *fp;

	if (profile_path(path, sizeof(path)) < 0)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "output %s\n", p->output);
	write_row(fp, "error", &p->model.error, 1);
	write_row(fp, "mean", p->model.mean, FEATURES);
	write_row(fp, "beta", p->model.beta, TERMS);
	for (int k = 0; k < n; k++) {
		fprintf(fp, "sample %d %.4f", s[k].point, (double)s[k].target);
		write_row(fp, "", s[k].f.v, FEATURES);
	}
	if (fclose(fp) != 0)
		return -1;
	return rename(tmp, path);
}

int
profile_load(Profile *p)
{
	char path[PATH_MAX];
	FILE *fp;
	int ok;

	if (profile_path(path, sizeof(path)) < 0)
		return -1;
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	ok = fscanf(fp, "output %63s", p->output) == 1 &&
		read_row(fp, "error", &p->model.error, 1) == 0 &&
		read_row(fp, "mean", p->model.mean, FEATURES) == 0 &&
		read_row(fp, "beta", p->model.beta, TERMS) == 0;
	fclose(fp);
	return ok ? 0 : -1;
}

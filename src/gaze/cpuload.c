#include <stdio.h>

#include "cpuload.h"

/* Busy share since last call. */
float
cpuload_sample(CpuLoad *l)
{
	unsigned long long v[8] = { 0 }, busy = 0, total = 0;
	FILE *fp = fopen("/proc/stat", "r");
	int n = fp ? fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
		&v[3], &v[4], &v[5], &v[6], &v[7]) : 0;
	float share;

	if (fp)
		fclose(fp);
	if (n < 5)
		return 0;
	for (int i = 0; i < n; i++)
		total += v[i];
	busy = total - v[3] - v[4];
	share = total > l->total ? (float)(busy - l->busy) / (float)(total - l->total) : 0;
	*l = (CpuLoad){ busy, total };
	return share;
}

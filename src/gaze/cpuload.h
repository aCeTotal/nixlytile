#ifndef GAZE_CPULOAD_H
#define GAZE_CPULOAD_H

typedef struct {
	unsigned long long busy, total;
} CpuLoad;

float cpuload_sample(CpuLoad *l);

#endif

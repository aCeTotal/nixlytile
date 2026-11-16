#ifndef TFLITE_H
#define TFLITE_H

#include <stddef.h>

typedef struct Net Net;

Net *net_load(const char *path);
void net_free(Net *net);
void net_run(Net *net);
float *net_input(Net *net, int i);
const float *net_output(const Net *net, int i);
size_t net_output_count(const Net *net, int i);

#endif

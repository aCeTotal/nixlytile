#ifndef GAZE_DECODE_H
#define GAZE_DECODE_H

#include "track.h"

typedef struct {
	void *tj;
	uint8_t *buf;
	Image img;
} Decoder;

int decoder_open(Decoder *d, int w, int h);
void decoder_close(Decoder *d);
const Image *decode(Decoder *d, const Frame *f, const Region *r);

#endif

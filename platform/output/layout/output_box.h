#ifndef PLATFORM_OUTPUT_LAYOUT_OUTPUT_BOX_H
#define PLATFORM_OUTPUT_LAYOUT_OUTPUT_BOX_H

#include "platform/output/layout.h"

// Output extents in layout coordinates.
void output_layout_output_get_box(struct nx_output_layout_output *l_output,
	struct nx_box *box);

#endif

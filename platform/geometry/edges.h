#ifndef PLATFORM_GEOMETRY_EDGES_H
#define PLATFORM_GEOMETRY_EDGES_H

enum nx_edges {
	NX_EDGE_NONE = 0,
	NX_EDGE_TOP = 1 << 0,
	NX_EDGE_BOTTOM = 1 << 1,
	NX_EDGE_LEFT = 1 << 2,
	NX_EDGE_RIGHT = 1 << 3,
};

#endif

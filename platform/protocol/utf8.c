#include <stddef.h>
#include <stdint.h>
#include "platform/protocol/utf8.h"

// Lead byte range, then continuation ranges.
static const uint8_t sequences[][8] = {
	{0x00, 0x7F},
	{0xC2, 0xDF, 0x80, 0xBF},
	{0xE0, 0xE0, 0xA0, 0xBF, 0x80, 0xBF},
	{0xE1, 0xEC, 0x80, 0xBF, 0x80, 0xBF},
	{0xED, 0xED, 0x80, 0x9F, 0x80, 0xBF},
	{0xEE, 0xEF, 0x80, 0xBF, 0x80, 0xBF},
	{0xF0, 0xF0, 0x90, 0xBF, 0x80, 0xBF, 0x80, 0xBF},
	{0xF1, 0xF3, 0x80, 0xBF, 0x80, 0xBF, 0x80, 0xBF},
	{0xF4, 0xF4, 0x80, 0x8F, 0x80, 0xBF, 0x80, 0xBF},
};

static const int lengths[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4 };

static bool in_range(char x, uint8_t low, uint8_t high) {
	uint8_t v = (uint8_t)x;
	return low <= v && v <= high;
}

// Zero for a malformed sequence.
static int sequence_length(const char *s) {
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		const uint8_t *range = sequences[i];
		if (!in_range(s[0], range[0], range[1])) {
			continue;
		}
		// Stops before reading past NUL.
		for (int j = 1; j < lengths[i]; j++) {
			if (!in_range(s[j], range[2 * j], range[2 * j + 1])) {
				return 0;
			}
		}
		return lengths[i];
	}
	return 0;
}

bool is_utf8(const char *string) {
	while (string[0]) {
		int len = sequence_length(string);
		if (len == 0) {
			return false;
		}
		string += len;
	}
	return true;
}

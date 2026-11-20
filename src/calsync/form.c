/* Stdin key=value input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "calsync.h"

#define ASCII_DEL 0x7f

static int
printable(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < ' ' || *s == ASCII_DEL)
			return 0;
	return 1;
}

/* Store one key=value line. */
static int
form_line(const Form *f, char *line)
{
	char *val;

	line[strcspn(line, "\n")] = 0;
	val = strchr(line, '=');
	if (!val)
		return 0;
	*val++ = 0;
	if (!printable(val))
		return -1;
	for (size_t i = 0; i < f->n; i++) {
		if (strcmp(line, f->keys[i].key))
			continue;
		if (strlen(val) >= f->keys[i].len)
			return -1;
		strcpy((char *)f->dst + f->keys[i].off, val);
		return 0;
	}
	return 0;
}

int
form_read(const Form *f)
{
	char *line = NULL;
	size_t cap = 0;
	int bad = 0;

	while (getline(&line, &cap, stdin) >= 0)
		bad |= form_line(f, line);
	if (line)
		explicit_bzero(line, cap);
	free(line);
	return bad;
}

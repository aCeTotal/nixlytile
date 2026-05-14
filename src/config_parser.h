/* config_parser.h — minimal KDL-subset parser for nixlytile runtime config.
 *
 * Supports:
 *   node "string-arg" 42 true prop="val" prop2=3.14 {
 *       child-node ...
 *   }
 *   // line comment
 *   (slash-star ... star-slash block comment)
 *
 * Values: quoted strings (with \" \\ \n \t \r escapes), integers (incl. 0x..),
 * floats (1.5, .5, 1e3), bools (true/false), null.
 *
 * Identifiers: [A-Za-z_][A-Za-z0-9_-]* . Strings may also be node/prop names
 * by quoting them.
 */
#ifndef NIXLYTILE_CONFIG_PARSER_H
#define NIXLYTILE_CONFIG_PARSER_H

#include <stddef.h>

typedef enum {
	KDL_VAL_NULL,
	KDL_VAL_STRING,
	KDL_VAL_INT,
	KDL_VAL_FLOAT,
	KDL_VAL_BOOL,
} KdlValueKind;

typedef struct {
	KdlValueKind kind;
	union {
		char  *s;     /* owned, NUL-terminated, for STRING; NULL for NULL */
		long   i;     /* INT or BOOL (0/1) */
		double f;     /* FLOAT */
	} u;
} KdlValue;

typedef struct KdlNode KdlNode;

typedef struct {
	char     *key;   /* owned */
	KdlValue  val;
} KdlProp;

struct KdlNode {
	char       *name;       /* owned */
	KdlValue   *args;
	size_t      n_args;
	KdlProp    *props;
	size_t      n_props;
	KdlNode    *children;
	size_t      n_children;
	int         line;       /* 1-based, for diagnostics */
};

typedef struct {
	KdlNode *roots;    /* top-level nodes */
	size_t   n_roots;
	char    *err;      /* NULL on success */
} KdlDoc;

/* Parse text (null-terminated). Returns a KdlDoc; check ->err for failure.
 * Caller must call kdl_doc_free() regardless. */
KdlDoc kdl_parse(const char *text);

void kdl_doc_free(KdlDoc *doc);

/* Convenience lookups (returns NULL / 0 if absent). */
const KdlNode *kdl_find_child(const KdlNode *parent, const char *name);
const KdlValue *kdl_get_prop(const KdlNode *node, const char *key);

/* Read first arg as a typed value. Returns 1 on success. */
int kdl_arg_string(const KdlNode *n, size_t idx, const char **out);
int kdl_arg_int(const KdlNode *n, size_t idx, long *out);
int kdl_arg_float(const KdlNode *n, size_t idx, double *out);
int kdl_arg_bool(const KdlNode *n, size_t idx, int *out);

/* Read a property (still falls back to first arg with same name semantics if
 * caller wants).  Returns 1 if present and convertible. */
int kdl_prop_string(const KdlNode *n, const char *key, const char **out);
int kdl_prop_int(const KdlNode *n, const char *key, long *out);
int kdl_prop_float(const KdlNode *n, const char *key, double *out);
int kdl_prop_bool(const KdlNode *n, const char *key, int *out);

#endif

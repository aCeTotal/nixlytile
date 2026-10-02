#include "nixlytile.h"

#define DA_LINE  1024
#define DA_ROOTS 8192
#define DA_SUFFIX ".desktop"
#define DA_SUFFIX_LEN (sizeof(DA_SUFFIX) - 1)

typedef struct {
	const char *home_var;
	const char *home_rel;
	const char *dirs_var;
	const char *dirs_def;
} XdgRoots;

typedef struct {
	const XdgRoots *roots;
	const char *file;
	const char *group;
} MimeSource;

typedef struct {
	const char *group;
	const char *key;
} IniKey;

static const XdgRoots config_roots = {
	"XDG_CONFIG_HOME", ".config", "XDG_CONFIG_DIRS", "/etc/xdg",
};
static const XdgRoots data_roots = {
	"XDG_DATA_HOME", ".local/share", "XDG_DATA_DIRS", "/usr/local/share:/usr/share",
};

/* User choice first, then caches. */
static const MimeSource sources[] = {
	{ &config_roots, "mimeapps.list", "Default Applications" },
	{ &data_roots, "applications/mimeapps.list", "Default Applications" },
	{ &data_roots, "applications/mimeinfo.cache", "MIME Cache" },
};

static void
roots_join(const XdgRoots *r, char out[DA_ROOTS])
{
	const char *home = getenv(r->home_var);
	const char *dirs = getenv(r->dirs_var);
	const char *user = getenv("HOME");

	if (!dirs || !*dirs)
		dirs = r->dirs_def;
	if (home && *home)
		snprintf(out, DA_ROOTS, "%s:%s", home, dirs);
	else
		snprintf(out, DA_ROOTS, "%s/%s:%s", user ? user : "", r->home_rel, dirs);
}

static int
is_group(const char *line, const char *group)
{
	size_t n = strlen(group);

	return !strncmp(line + 1, group, n) && line[n + 1] == ']' && !line[n + 2];
}

static int
ini_value(const char *path, IniKey k, char out[DA_LINE])
{
	FILE *f = fopen(path, "r");
	char line[DA_LINE];
	size_t klen = strlen(k.key);
	int in_group = 0;
	int found = 0;

	if (!f)
		return 0;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_group = is_group(line, k.group);
		} else if (in_group && !strncmp(line, k.key, klen) && line[klen] == '=') {
			snprintf(out, DA_LINE, "%s", line + klen + 1);
			found = 1;
		}
	}
	fclose(f);
	return found;
}

static int
desktop_path(const char *id, char out[PATH_MAX])
{
	char roots[DA_ROOTS];
	char *save = NULL;

	roots_join(&data_roots, roots);
	for (char *r = strtok_r(roots, ":", &save); r; r = strtok_r(NULL, ":", &save)) {
		snprintf(out, PATH_MAX, "%s/applications/%s", r, id);
		if (access(out, R_OK) == 0)
			return 1;
	}
	return 0;
}

/* First installed of "a.desktop;b.desktop;". */
static int
first_installed(char *ids, char out[PATH_MAX])
{
	char *save = NULL;

	for (char *id = strtok_r(ids, ";", &save); id; id = strtok_r(NULL, ";", &save))
		if (desktop_path(id, out))
			return 1;
	return 0;
}

static int
source_lookup(const MimeSource *s, const char *mime, char out[PATH_MAX])
{
	char roots[DA_ROOTS];
	char list[PATH_MAX];
	char ids[DA_LINE];
	char *save = NULL;

	roots_join(s->roots, roots);
	for (char *r = strtok_r(roots, ":", &save); r; r = strtok_r(NULL, ":", &save)) {
		snprintf(list, sizeof(list), "%s/%s", r, s->file);
		if (ini_value(list, (IniKey){ s->group, mime }, ids) && first_installed(ids, out))
			return 1;
	}
	return 0;
}

/* Drops %u, %F and friends. */
static void
strip_field_codes(char *exec)
{
	char *w = exec;

	for (const char *r = exec; *r; r++) {
		if (*r != '%') {
			*w++ = *r;
			continue;
		}
		if (r[1] == '%')
			*w++ = '%';
		if (r[1])
			r++;
	}
	*w = '\0';
}

int
desktop_exec(const char *path, char *out, size_t size)
{
	char exec[DA_LINE];

	if (!ini_value(path, (IniKey){ "Desktop Entry", "Exec" }, exec))
		return 0;
	strip_field_codes(exec);
	return snprintf(out, size, "%s", exec) < (int)size;
}

int
default_app_path(const char *mime, char out[PATH_MAX])
{
	for (size_t i = 0; i < LENGTH(sources); i++)
		if (source_lookup(&sources[i], mime, out))
			return 1;
	return 0;
}

int
default_app_exec(const char *mime, char *out, size_t size)
{
	char path[PATH_MAX];

	return default_app_path(mime, path) && desktop_exec(path, out, size);
}

static void
app_names_add(AppNames *a, const char *name, size_t len)
{
	if (!len || a->n == APP_NAMES)
		return;
	snprintf(a->name[a->n++], APP_NAME_MAX, "%.*s", (int)len, name);
}

/* Basename of the first word. */
void
app_names_add_program(AppNames *a, const char *exec)
{
	size_t len = strcspn(exec, " \t");
	const char *base = exec;

	for (const char *p = exec; p < exec + len; p++)
		if (*p == '/')
			base = p + 1;
	app_names_add(a, base, len - (size_t)(base - exec));
}

void
app_names_from_desktop(const char *path, AppNames *a)
{
	const char *id = strrchr(path, '/');
	char line[DA_LINE];
	size_t len;

	id = id ? id + 1 : path;
	len = strlen(id);
	if (len > DA_SUFFIX_LEN && !strcmp(id + len - DA_SUFFIX_LEN, DA_SUFFIX))
		len -= DA_SUFFIX_LEN;
	app_names_add(a, id, len);
	if (ini_value(path, (IniKey){ "Desktop Entry", "StartupWMClass" }, line))
		app_names_add(a, line, strlen(line));
	if (ini_value(path, (IniKey){ "Desktop Entry", "Exec" }, line))
		app_names_add_program(a, line);
}

int
app_names_match(const AppNames *a, const char *app_id)
{
	for (int i = 0; i < a->n; i++)
		if (!strcasecmp(a->name[i], app_id))
			return 1;
	return 0;
}

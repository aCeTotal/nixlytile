#include "nixlytile.h"
#include "client.h"
#include "game_launch_params.h"

static GameEntry *
find_game_by_id(GameEntry *head, const char *id, int match_service, GamingServiceType service)
{
	GameEntry *g = head;
	while (g) {
		if (strcmp(g->id, id) == 0 &&
				(!match_service || g->service == service)) {
			return g;
		}
		g = g->next;
	}
	return NULL;
}

const char *retro_console_names[] = {
	"NES", "SNES", "Nintendo 64", "GameCube", "Wii",
	"Game Boy", "Game Boy Color", "Game Boy Advance"
};

/* Emulator launch commands per console (hardcoded fallback) */
static const char *retro_emulator_cmds[] = {
	[RETRO_NES]      = "Mesen \"%s\"",
	[RETRO_SNES]     = "Mesen \"%s\"",
	[RETRO_N64]      = "RMG \"%s\" -f",
	[RETRO_GAMECUBE] = "dolphin-emu -e \"%s\" -b",
	[RETRO_WII]      = "dolphin-emu -e \"%s\" -b",
	[RETRO_GB]       = "mgba -f \"%s\"",
	[RETRO_GBC]      = "mgba -f \"%s\"",
	[RETRO_GBA]      = "mgba -f \"%s\"",
};

/* User-configured emulator overrides from config.conf */
#define MAX_EMULATORS 32
static struct {
	char tag[32];     /* "nes", "snes", etc. */
	char cmd[512];    /* command template with %s for ROM path */
} retro_emu_config[MAX_EMULATORS];
static int retro_emu_config_count;

void
config_set_emulator(const char *value)
{
	char tag[32];
	const char *p = value;
	int i = 0;

	/* Parse: "nes mesen \"%s\"" → tag="nes", cmd="mesen \"%s\"" */
	while (*p && *p != ' ' && *p != '\t' && i < (int)sizeof(tag) - 1)
		tag[i++] = *p++;
	tag[i] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;
	if (!tag[0] || !*p)
		return;

	/* Check if tag already exists → overwrite */
	for (i = 0; i < retro_emu_config_count; i++) {
		if (strcmp(retro_emu_config[i].tag, tag) == 0) {
			snprintf(retro_emu_config[i].cmd, sizeof(retro_emu_config[i].cmd), "%s", p);
			return;
		}
	}
	if (retro_emu_config_count >= MAX_EMULATORS)
		return;
	snprintf(retro_emu_config[retro_emu_config_count].tag, sizeof(retro_emu_config[0].tag), "%s", tag);
	snprintf(retro_emu_config[retro_emu_config_count].cmd, sizeof(retro_emu_config[0].cmd), "%s", p);
	retro_emu_config_count++;
}

static const char *
retro_get_emulator_cmd(const char *tag)
{
	for (int i = 0; i < retro_emu_config_count; i++)
		if (strcmp(retro_emu_config[i].tag, tag) == 0)
			return retro_emu_config[i].cmd;
	return NULL;
}

void
pc_gaming_cache_update_start(void)
{
	pid_t pid;
	char *home;

	home = getenv("HOME");
	if (!home)
		return;

	pid = fork();
	if (pid == 0) {
		setsid();
		fork_detach();
		/* Update PC gaming cache in background with low priority - scans Steam and fetches game info */
		execlp("nice", "nice", "-n", "19", "ionice", "-c", "3", "sh", "-c",
			"home=\"$HOME\"\n"
			"cache=\"${XDG_CACHE_HOME:-$home/.cache}/nixlytile/games.cache\"\n"
			"mkdir -p \"$(dirname \"$cache\")\"\n"
			"steam_root=\"$home/.local/share/Steam\"\n"
			"tmp=$(mktemp)\n"
			"trap 'rm -f \"$tmp\" \"$tmp.api\" \"$tmp.playtime\" \"$tmp.lastplayed\" \"$tmp.installed\" \"$tmp.script\"' EXIT\n"
			"\n"
			"# Find all app ID directories with portrait images only\n"
			"for d in \"$steam_root/appcache/librarycache\"/[0-9]*; do\n"
			"  [ -d \"$d\" ] || continue\n"
			"  appid=$(basename \"$d\")\n"
			"  [ -f \"$d/library_600x900.jpg\" ] || continue\n"
			"  echo \"$appid\" >> \"$tmp\"\n"
			"done\n"
			"\n"
			"# Get installed apps\n"
			"for f in \"$steam_root/steamapps\"/appmanifest_*.acf; do\n"
			"  [ -f \"$f\" ] || continue\n"
			"  basename \"$f\" | sed 's/appmanifest_//;s/.acf//'\n"
			"done > \"$tmp.installed\"\n"
			"\n"
			"# Find userdata directory (use first found)\n"
			"userdir=$(ls -d \"$steam_root/userdata\"/[0-9]* 2>/dev/null | head -1)\n"
			"lcfg=\"$userdir/config/localconfig.vdf\"\n"
			"# Parse playtime and LastPlayed from localconfig.vdf\n"
			"if [ -f \"$lcfg\" ]; then\n"
			"  awk '/^[[:space:]]*\"[0-9]+\"[[:space:]]*$/{gsub(/\\\"/,\"\",$0);gsub(/[[:space:]]/,\"\",$0);if($0~/^[0-9]+$/)id=$0} /\"Playtime\"/{split($0,a,\"\\\"\");if(id)print id\"|\"a[4]}' \"$lcfg\" > \"$tmp.playtime\"\n"
			"  awk '/^[[:space:]]*\"[0-9]+\"[[:space:]]*$/{gsub(/\\\"/,\"\",$0);gsub(/[[:space:]]/,\"\",$0);if($0~/^[0-9]+$/)id=$0} /\"LastPlayed\"/{split($0,a,\"\\\"\");v=a[4];if(id && v>86400)print id\"|\"v}' \"$lcfg\" > \"$tmp.lastplayed\"\n"
			"fi\n"
			"\n"
			"# Fetch from Steam API - name, type, controller support, deck status\n"
			"rm -f \"$tmp.api\"\n"
			"touch \"$tmp.api\"\n"
			"count=0\n"
			"while read -r appid; do\n"
			"  [ -z \"$appid\" ] && continue\n"
			"  echo \"(r=\\$(curl -s --max-time 8 -A 'Mozilla/5.0' 'https://store.steampowered.com/api/appdetails?appids=$appid');\"\n"
			"  echo \" n=\\$(echo \\\"\\$r\\\" | grep -o '\\\"name\\\":\\\"[^\\\"]*\\\"' | head -1 | sed 's/\\\"name\\\":\\\"//;s/\\\"\\$//');\"\n"
			"  echo \" t=\\$(echo \\\"\\$r\\\" | grep -o '\\\"type\\\":\\\"[^\\\"]*\\\"' | head -1 | sed 's/\\\"type\\\":\\\"//;s/\\\"\\$//');\"\n"
			"  echo \" ctrl=0; echo \\\"\\$r\\\" | grep -q '\\\"id\\\":28' && ctrl=2; echo \\\"\\$r\\\" | grep -q '\\\"id\\\":18' && [ \\$ctrl -eq 0 ] && ctrl=1;\"\n"
			"  echo \" deck=0; dr=\\$(curl -s --max-time 5 -A 'Mozilla/5.0' 'https://store.steampowered.com/saleaction/ajaxgetdeckappcompatibilityreport?nAppID=$appid');\"\n"
			"  echo \" case \\\"\\$dr\\\" in *\\\"category\\\":3*) deck=3;; *\\\"category\\\":2*) deck=2;; *\\\"category\\\":1*) deck=1;; esac;\"\n"
			"  echo \" [ -n \\\"\\$n\\\" ] && echo '$appid|'\\\"\\$n\\\"'|'\\\"\\$t\\\"'|'\\\"\\$ctrl\\\"'|'\\\"\\$deck\\\" >> '$tmp.api') &\"\n"
			"  count=$((count+1))\n"
			"  if [ $count -ge 25 ]; then\n"
			"    echo 'wait'\n"
			"    count=0\n"
			"  fi\n"
			"done < \"$tmp\" > \"$tmp.script\"\n"
			"echo 'wait' >> \"$tmp.script\"\n"
			"sh \"$tmp.script\" 2>/dev/null\n"
			"\n"
			"# Build cache - only deck-ready games with controller support\n"
			"games=0\n"
			"output=$(mktemp)\n"
			"while read -r appid; do\n"
			"  [ -z \"$appid\" ] && continue\n"
			"  line=$(grep \"^$appid|\" \"$tmp.api\" 2>/dev/null | head -1)\n"
			"  [ -z \"$line\" ] && continue\n"
			"  name=$(echo \"$line\" | cut -d'|' -f2)\n"
			"  type=$(echo \"$line\" | cut -d'|' -f3)\n"
			"  ctrl=$(echo \"$line\" | cut -d'|' -f4)\n"
			"  deck=$(echo \"$line\" | cut -d'|' -f5)\n"
			"  [ -z \"$name\" ] && continue\n"
			"  [ -z \"$ctrl\" ] && ctrl=0\n"
			"  [ -z \"$deck\" ] && deck=0\n"
			"  # Skip if no controller support or not deck playable/verified\n"
			"  [ \"$ctrl\" -lt 1 ] 2>/dev/null && continue\n"
			"  [ \"$deck\" -lt 2 ] 2>/dev/null && continue\n"
			"  icon=\"$steam_root/appcache/librarycache/${appid}/library_600x900.jpg\"\n"
			"  launch=\"steam steam://rungameid/$appid\"\n"
			"  installed=0; grep -qx \"$appid\" \"$tmp.installed\" 2>/dev/null && installed=1\n"
			"  playtime=0; pt=$(grep \"^$appid|\" \"$tmp.playtime\" 2>/dev/null | cut -d'|' -f2); [ -n \"$pt\" ] && playtime=$pt\n"
			"  acquired=0; lp=$(grep \"^$appid|\" \"$tmp.lastplayed\" 2>/dev/null | cut -d'|' -f2)\n"
			"  [ -n \"$lp\" ] && [ \"$lp\" -gt 86400 ] 2>/dev/null && acquired=$lp\n"
			"  is_game=1\n"
			"  case \"$type\" in dlc|tool|demo|mod|video|music|advertising|series|episode|hardware) is_game=0;; esac\n"
			"  case \"$name\" in *Proton*|*'Dedicated Server'*|*SDK*|*Runtime*|*Redistributable*|*Steamworks*|*Soundtrack*) is_game=0;; esac\n"
			"  [ $is_game -eq 0 ] && continue\n"
			"  echo \"$appid|$name|$icon|$launch|0|$installed|$playtime|$is_game|$acquired|$ctrl|$deck\" >> \"$output\"\n"
			"  games=$((games+1))\n"
			"done < \"$tmp\"\n"
			"\n"
			"{ echo 'NIXLYTILE_GAMES_CACHE_V7'; echo \"$games\"; sort -t'|' -k9 -rn \"$output\"; } > \"$cache.tmp\" && mv \"$cache.tmp\" \"$cache\"\n"
			"rm -f \"$output\"\n",
			(char *)NULL);
		_exit(127);
	} else if (pid > 0) {
		wlr_log(WLR_INFO, "pc gaming cache update started: pid=%d", pid);
	}
}

void
pc_gaming_free_games(Monitor *m)
{
	GameEntry *g, *next;

	if (!m)
		return;

	g = m->pc_gaming.games;
	while (g) {
		next = g->next;
		if (g->icon_buf)
			wlr_buffer_drop(g->icon_buf);
		free(g);
		g = next;
	}
	m->pc_gaming.games = NULL;
	m->pc_gaming.game_count = 0;
}

void
pc_gaming_add_game(Monitor *m, const char *id, const char *name, const char *launch_cmd,
                   const char *icon_path, GamingServiceType service, int installed)
{
	GameEntry *g;

	if (!m || !id || !name)
		return;

	g = calloc(1, sizeof(*g));
	if (!g)
		return;

	snprintf(g->id, sizeof(g->id), "%s", id);
	snprintf(g->name, sizeof(g->name), "%s", name);
	snprintf(g->launch_cmd, sizeof(g->launch_cmd), "%s", launch_cmd ? launch_cmd : "");
	if (icon_path && icon_path[0])
		snprintf(g->icon_path, sizeof(g->icon_path), "%s", icon_path);
	g->service = service;
	g->installed = installed;
	g->is_game = 1;  /* Games added via this function are actual games */
	g->icon_buf = NULL;
	g->icon_loaded = 0;

	/* Add to front of list */
	g->next = m->pc_gaming.games;
	m->pc_gaming.games = g;
	m->pc_gaming.game_count++;
}

void
pc_gaming_save_cache(Monitor *m)
{
	FILE *fp;
	char path[512];
	char *home;
	GameEntry *g;

	if (!m)
		return;

	home = getenv("HOME");
	if (!home)
		return;

	/* Create cache directory */
	snprintf(path, sizeof(path), "%s/.cache/nixlytile", home);
	mkdir(path, 0755);

	snprintf(path, sizeof(path), "%s" PC_GAMING_CACHE_FILE, home);
	fp = fopen(path, "w");
	if (!fp)
		return;

	/* Write header with version */
	fprintf(fp, "NIXLYTILE_GAMES_CACHE_V7\n");
	fprintf(fp, "%d\n", m->pc_gaming.game_count);

	/* Write each game: id|name|icon_path|launch_cmd|service|installed|playtime|is_game|acquired|controller|deck */
	g = m->pc_gaming.games;
	while (g) {
		fprintf(fp, "%s|%s|%s|%s|%d|%d|%d|%d|%ld|%d|%d\n",
			g->id, g->name, g->icon_path, g->launch_cmd,
			(int)g->service, g->installed, g->playtime_minutes, g->is_game,
			(long)g->acquired_time, g->controller_support, g->deck_verified);
		g = g->next;
	}

	fclose(fp);
	wlr_log(WLR_INFO, "Saved %d games to cache", m->pc_gaming.game_count);
}

int
pc_gaming_load_cache(Monitor *m)
{
	FILE *fp;
	char path[512];
	char line[2048];
	char *home;
	int count = 0;
	struct stat st;

	if (!m)
		return 0;

	home = getenv("HOME");
	if (!home)
		return 0;

	snprintf(path, sizeof(path), "%s" PC_GAMING_CACHE_FILE, home);

	/* Check if cache exists and is recent (less than 24 hours old) */
	if (stat(path, &st) != 0)
		return 0;

	time_t now = time(NULL);
	if (now - st.st_mtime > 86400) {
		wlr_log(WLR_INFO, "Games cache is stale, will refresh");
		return 0;
	}

	fp = fopen(path, "r");
	if (!fp)
		return 0;

	/* Check header */
	if (!fgets(line, sizeof(line), fp) ||
	    strncmp(line, "NIXLYTILE_GAMES_CACHE_V7", 24) != 0) {
		fclose(fp);
		return 0;
	}

	/* Read count */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return 0;
	}
	count = atoi(line);
	if (count <= 0 || count > 10000) {
		fclose(fp);
		return 0;
	}

	/* Read games */
	while (fgets(line, sizeof(line), fp)) {
		char *id, *name, *icon, *launch, *svc_str, *inst_str, *playtime_str, *is_game_str, *acquired_str;
		char *saveptr;

		/* Remove newline */
		line[strcspn(line, "\n")] = '\0';

		id = strtok_r(line, "|", &saveptr);
		name = strtok_r(NULL, "|", &saveptr);
		icon = strtok_r(NULL, "|", &saveptr);
		launch = strtok_r(NULL, "|", &saveptr);
		svc_str = strtok_r(NULL, "|", &saveptr);
		inst_str = strtok_r(NULL, "|", &saveptr);
		playtime_str = strtok_r(NULL, "|", &saveptr);
		is_game_str = strtok_r(NULL, "|", &saveptr);
		acquired_str = strtok_r(NULL, "|", &saveptr);
		char *controller_str = strtok_r(NULL, "|", &saveptr);
		char *deck_str = strtok_r(NULL, "|", &saveptr);

		if (!id || !name || !svc_str)
			continue;

		GameEntry *g = calloc(1, sizeof(*g));
		if (!g)
			continue;

		snprintf(g->id, sizeof(g->id), "%s", id);
		snprintf(g->name, sizeof(g->name), "%s", name);
		if (icon)
			snprintf(g->icon_path, sizeof(g->icon_path), "%s", icon);
		if (launch)
			snprintf(g->launch_cmd, sizeof(g->launch_cmd), "%s", launch);
		g->service = (GamingServiceType)atoi(svc_str);
		g->installed = inst_str ? atoi(inst_str) : 0;
		g->playtime_minutes = playtime_str ? atoi(playtime_str) : 0;
		g->is_game = is_game_str ? atoi(is_game_str) : 1;  /* Default to game if not specified */
		g->acquired_time = acquired_str ? atol(acquired_str) : 0;
		g->controller_support = controller_str ? atoi(controller_str) : 0;
		g->deck_verified = deck_str ? atoi(deck_str) : 0;
		g->icon_buf = NULL;
		g->icon_loaded = 0;

		g->next = m->pc_gaming.games;
		m->pc_gaming.games = g;
		m->pc_gaming.game_count++;
	}

	fclose(fp);
	wlr_log(WLR_INFO, "Loaded %d games from cache", m->pc_gaming.game_count);
	return m->pc_gaming.game_count > 0;
}

void
pc_gaming_fetch_steam_names_batch(Monitor *m)
{
	FILE *script_fp, *result_fp;
	char script_path[128];
	char result_path[128];
	char *buf = NULL;
	size_t buf_size = 0;
	int need_fetch = 0;
	GameEntry *g;

	if (!m)
		return;

	/* Create temp files */
	snprintf(script_path, sizeof(script_path), "/tmp/nixlytile_steam_%d.sh", getpid());
	snprintf(result_path, sizeof(result_path), "/tmp/nixlytile_steam_%d.txt", getpid());

	script_fp = fopen(script_path, "w");
	if (!script_fp)
		return;

	/* Write script header */
	fprintf(script_fp, "#!/bin/sh\n");

	/* Write parallel fetch commands for each game needing a name */
	g = m->pc_gaming.games;
	while (g) {
		if (g->service == GAMING_SERVICE_STEAM &&
		    strncmp(g->name, "Steam Game ", 11) == 0) {
			fprintf(script_fp,
				"(resp=$(curl -s --max-time 3 -A 'Mozilla/5.0' 'https://store.steampowered.com/api/appdetails?appids=%s' 2>/dev/null)\n"
				" name=$(echo \"$resp\" | grep -o '\"name\":\"[^\"]*\"' | head -1 | sed 's/\"name\":\"//;s/\"$//')\n"
				" type=$(echo \"$resp\" | grep -o '\"type\":\"[^\"]*\"' | head -1 | sed 's/\"type\":\"//;s/\"$//')\n"
				" if [ -n \"$name\" ]; then echo '%s|'\"$name\"'|'\"$type\"; fi) &\n",
				g->id, g->id);
			need_fetch++;
		}
		g = g->next;
	}

	if (need_fetch == 0) {
		fclose(script_fp);
		unlink(script_path);
		return;
	}

	fprintf(script_fp, "wait\n");
	fclose(script_fp);

	wlr_log(WLR_INFO, "Fetching names for %d Steam games in parallel...", need_fetch);

	/* Execute script */
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "sh %s > %s 2>/dev/null", script_path, result_path);
	int ret = system(cmd);
	unlink(script_path);

	if (ret != 0) {
		unlink(result_path);
		return;
	}

	/* Read results */
	buf = read_file_to_string(result_path, &buf_size);
	unlink(result_path);

	if (!buf || buf_size == 0) {
		free(buf);
		return;
	}

	/* Parse results: "appid|Game Name|type\n" */
	int updated = 0;
	char *line = buf;
	while (line && *line) {
		char *newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		char *sep1 = strchr(line, '|');
		if (sep1) {
			*sep1 = '\0';
			char *appid_str = line;
			char *name = sep1 + 1;
			char *type = NULL;

			char *sep2 = strchr(name, '|');
			if (sep2) {
				*sep2 = '\0';
				type = sep2 + 1;
			}

			/* Find and update game entry */
			g = m->pc_gaming.games;
			while (g) {
				if (g->service == GAMING_SERVICE_STEAM &&
				    strcmp(g->id, appid_str) == 0) {
					snprintf(g->name, sizeof(g->name), "%s", name);
					/* Mark as NOT a game only for specific non-game types */
					if (type && (strcmp(type, "dlc") == 0 ||
					             strcmp(type, "tool") == 0 ||
					             strcmp(type, "demo") == 0 ||
					             strcmp(type, "advertising") == 0 ||
					             strcmp(type, "mod") == 0 ||
					             strcmp(type, "video") == 0 ||
					             strcmp(type, "hardware") == 0 ||
					             strcmp(type, "series") == 0 ||
					             strcmp(type, "episode") == 0)) {
						g->is_game = 0;
					}
					updated++;
					break;
				}
				g = g->next;
			}
		}

		line = newline ? newline + 1 : NULL;
	}

	free(buf);
	wlr_log(WLR_INFO, "Updated %d game names from Steam API", updated);
}

void
pc_gaming_load_steam_playtime(Monitor *m)
{
	FILE *fp;
	char path[512];
	char line[1024];
	char *home;
	DIR *dir;
	struct dirent *ent;
	char current_appid[32] = "";
	int in_apps_section = 0;

	if (!m)
		return;

	home = getenv("HOME");
	if (!home)
		return;

	/* Find Steam user directory */
	snprintf(path, sizeof(path), "%s/.local/share/Steam/userdata", home);
	dir = opendir(path);
	if (!dir)
		return;

	/* Find first user ID directory */
	char userid[32] = "";
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] != '.' && ent->d_type == DT_DIR) {
			snprintf(userid, sizeof(userid), "%s", ent->d_name);
			break;
		}
	}
	closedir(dir);

	if (!userid[0])
		return;

	snprintf(path, sizeof(path), "%s/.local/share/Steam/userdata/%s/config/localconfig.vdf", home, userid);
	fp = fopen(path, "r");
	if (!fp)
		return;

	/* Parse VDF format for playtime */
	while (fgets(line, sizeof(line), fp)) {
		/* Look for "apps" section */
		if (strstr(line, "\"apps\""))
			in_apps_section = 1;

		if (!in_apps_section)
			continue;

		/* Check for app ID (numeric key) */
		char *quote1 = strchr(line, '"');
		if (quote1) {
			char *quote2 = strchr(quote1 + 1, '"');
			if (quote2) {
				size_t len = quote2 - quote1 - 1;
				if (len > 0 && len < sizeof(current_appid)) {
					/* Check if it's a number (appid) */
					int is_num = 1;
					for (size_t i = 0; i < len; i++) {
						if (quote1[1 + i] < '0' || quote1[1 + i] > '9') {
							is_num = 0;
							break;
						}
					}
					if (is_num) {
						memcpy(current_appid, quote1 + 1, len);
						current_appid[len] = '\0';
					}
				}
			}
		}

		/* Look for Playtime in current app context */
		if (current_appid[0] && strstr(line, "\"Playtime\"")) {
			char *val_start = strstr(line, "\"Playtime\"");
			val_start = strchr(val_start + 10, '"');
			if (val_start) {
				val_start++;
				int playtime = atoi(val_start);

				GameEntry *g = find_game_by_id(m->pc_gaming.games,
						current_appid, 1, GAMING_SERVICE_STEAM);
				if (g)
					g->playtime_minutes = playtime;
			}
		}

		/* Look for LastPlayed in current app context - use as acquired_time */
		if (current_appid[0] && strstr(line, "\"LastPlayed\"")) {
			char *val_start = strstr(line, "\"LastPlayed\"");
			val_start = strchr(val_start + 12, '"');
			if (val_start) {
				val_start++;
				long lastplayed = atol(val_start);

				/* Only use if > 86400 (skip placeholder values) */
				if (lastplayed > 86400) {
					GameEntry *g = find_game_by_id(m->pc_gaming.games,
							current_appid, 1, GAMING_SERVICE_STEAM);
					if (g)
						g->acquired_time = (time_t)lastplayed;
				}
			}
		}
	}

	fclose(fp);
}

void
pc_gaming_split_list(GameEntry *source, GameEntry **front, GameEntry **back)
{
	GameEntry *fast, *slow;
	slow = source;
	fast = source->next;

	while (fast) {
		fast = fast->next;
		if (fast) {
			slow = slow->next;
			fast = fast->next;
		}
	}

	*front = source;
	*back = slow->next;
	slow->next = NULL;
}

void
pc_gaming_merge_sort(GameEntry **head)
{
	GameEntry *h = *head;
	GameEntry *a, *b;

	if (!h || !h->next)
		return;

	pc_gaming_split_list(h, &a, &b);
	pc_gaming_merge_sort(&a);
	pc_gaming_merge_sort(&b);

	*head = pc_gaming_merge_sorted(a, b);
}

void
pc_gaming_sort_by_acquired(Monitor *m)
{
	if (!m || !m->pc_gaming.games)
		return;

	pc_gaming_merge_sort(&m->pc_gaming.games);
	wlr_log(WLR_INFO, "Sorted games: played first (by recency), then unplayed (by age)");
}

int pc_gaming_is_known_tool(const char *appid)
{
	/* Proton versions */
	static const char *proton_ids[] = {
		"1887720", "2180100", "1493710", "1580130", "2348590", "1245040",
		"961940", "1054830", "1113280", "1420170", "858280", "930400",
		"996510", "2805730", "2230260", NULL
	};
	/* Steam Linux Runtime */
	static const char *runtime_ids[] = {
		"1070560", "1391110", "1628350", NULL
	};
	/* Steamworks and redistributables */
	static const char *redist_ids[] = {
		"228980", "228981", "1007", NULL
	};
	for (int i = 0; proton_ids[i]; i++)
		if (strcmp(appid, proton_ids[i]) == 0) return 1;
	for (int i = 0; runtime_ids[i]; i++)
		if (strcmp(appid, runtime_ids[i]) == 0) return 1;
	for (int i = 0; redist_ids[i]; i++)
		if (strcmp(appid, redist_ids[i]) == 0) return 1;
	return 0;
}

void
pc_gaming_filter_non_games(Monitor *m)
{
	GameEntry **pp, *g;
	int removed = 0;

	if (!m)
		return;

	pp = &m->pc_gaming.games;
	while (*pp) {
		g = *pp;
		int should_filter = !g->is_game;

		/* Don't filter games without proper names - show with app ID instead */
		/* API may fail due to network issues, but game is still valid */

		/* Filter known Proton/runtime/tool app IDs */
		if (!should_filter && pc_gaming_is_known_tool(g->id))
			should_filter = 1;

		/* Also filter by name patterns for tools/servers/SDKs */
		if (!should_filter && g->name[0]) {
			if (strstr(g->name, "Proton") ||
			    strstr(g->name, "Dedicated Server") ||
			    strstr(g->name, "SDK") ||
			    strstr(g->name, "Mod Tools") ||
			    strstr(g->name, "Content Editor") ||
			    strstr(g->name, "Runtime") ||
			    strstr(g->name, "Redistributable") ||
			    strstr(g->name, "Steamworks") ||
			    strstr(g->name, "Soundtrack") ||
			    strstr(g->name, "Wallpaper") ||
			    strstr(g->name, "Artbook"))
				should_filter = 1;
		}
		if (should_filter) {
			*pp = g->next;
			if (g->icon_buf)
				wlr_buffer_drop(g->icon_buf);
			free(g);
			m->pc_gaming.game_count--;
			removed++;
		} else {
			pp = &g->next;
		}
	}

	wlr_log(WLR_INFO, "Filtered out %d non-game entries", removed);
}

void
pc_gaming_update_install_status(Monitor *m)
{
	GameEntry *g;
	char *home;
	const char *steam_paths[] = {
		"%s/.local/share/Steam/steamapps",
		"%s/.steam/steam/steamapps",
		NULL
	};

	if (!m)
		return;

	home = getenv("HOME");
	if (!home)
		return;

	g = m->pc_gaming.games;
	while (g) {
		if (g->service == GAMING_SERVICE_STEAM) {
			/* Check appmanifest for download status */
			for (int i = 0; steam_paths[i]; i++) {
				char manifest_path[512];
				FILE *fp;

				snprintf(manifest_path, sizeof(manifest_path),
					steam_paths[i], home);
				snprintf(manifest_path + strlen(manifest_path),
					sizeof(manifest_path) - strlen(manifest_path),
					"/appmanifest_%s.acf", g->id);

				fp = fopen(manifest_path, "r");
				if (fp) {
					char line[512];
					int64_t bytes_to_download = 0;
					int64_t bytes_downloaded = 0;
					int state_flags = 0;

					while (fgets(line, sizeof(line), fp)) {
						char *key, *val;
						key = strstr(line, "\"StateFlags\"");
						if (key) {
							val = strchr(key + 12, '\"');
							if (val) {
								val++;
								state_flags = atoi(val);
							}
						}
						key = strstr(line, "\"BytesToDownload\"");
						if (key) {
							val = strchr(key + 17, '\"');
							if (val) {
								val++;
								bytes_to_download = atoll(val);
							}
						}
						key = strstr(line, "\"BytesDownloaded\"");
						if (key) {
							val = strchr(key + 17, '\"');
							if (val) {
								val++;
								bytes_downloaded = atoll(val);
							}
						}
					}
					fclose(fp);

					/* StateFlags: 1026 = downloading, 1042 = updating, 4 = installed */
					if ((state_flags & 1024) && bytes_to_download > 0) {
						g->is_installing = 1;
						g->install_progress = (int)((bytes_downloaded * 100) / bytes_to_download);
						if (g->install_progress > 100)
							g->install_progress = 100;
					} else if (state_flags == 4) {
						/* Fully installed */
						g->is_installing = 0;
						g->install_progress = 0;
						g->installed = 1;
					}
					/* Don't reset is_installing for other states - Steam might be
					 * preparing the download. Only clear when confirmed installed. */
					break;
				}
			}
		}
		/* TODO: Add Heroic/Epic installation progress support */
		g = g->next;
	}
}

void
pc_gaming_load_game_icon(GameEntry *g, int target_w, int target_h)
{
	GdkPixbuf *pixbuf = NULL, *scaled = NULL;
	GError *gerr = NULL;
	int w, h, nchan, stride;
	guchar *pixels;
	uint8_t *argb = NULL;
	size_t bufsize;

	if (!g || g->icon_loaded)
		return;

	g->icon_loaded = 1;  /* Mark as attempted */

	if (!g->icon_path[0] || access(g->icon_path, F_OK) != 0)
		return;

	pixbuf = gdk_pixbuf_new_from_file(g->icon_path, &gerr);
	if (!pixbuf) {
		if (gerr) {
			g_error_free(gerr);
		}
		return;
	}

	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0) {
		g_object_unref(pixbuf);
		return;
	}

	/* Scale to exact target dimensions (fill/crop) */
	if (w != target_w || h != target_h) {
		scaled = gdk_pixbuf_scale_simple(pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
		g_object_unref(pixbuf);
		if (!scaled)
			return;
		pixbuf = scaled;
		w = target_w;
		h = target_h;
	}

	nchan = gdk_pixbuf_get_n_channels(pixbuf);
	if (nchan < 3) {
		g_object_unref(pixbuf);
		return;
	}
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (!pixels) {
		g_object_unref(pixbuf);
		return;
	}

	bufsize = (size_t)w * (size_t)h * 4;
	argb = calloc(1, bufsize);
	if (!argb) {
		g_object_unref(pixbuf);
		return;
	}

	/* Convert RGB/RGBA to ARGB32 for wlroots */
	for (int y = 0; y < h; y++) {
		const guchar *row = pixels + y * stride;
		uint32_t *dst = (uint32_t *)(argb + y * w * 4);
		for (int x = 0; x < w; x++) {
			uint8_t r = row[x * nchan + 0];
			uint8_t gg = row[x * nchan + 1];
			uint8_t b = row[x * nchan + 2];
			uint8_t a = (nchan >= 4) ? row[x * nchan + 3] : 255;
			dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			         ((uint32_t)gg << 8) | (uint32_t)b;
		}
	}

	g_object_unref(pixbuf);

	g->icon_buf = statusbar_buffer_from_argb32_raw((uint32_t *)argb, w, h);
	free(argb);

	if (g->icon_buf) {
		g->icon_w = w;
		g->icon_h = h;
	}
}

void
pc_gaming_scan_steam(Monitor *m)
{
	char path[512];
	char icon_path[768];
	char *home;
	DIR *dir;
	struct dirent *ent;

	if (!m)
		return;

	home = getenv("HOME");
	if (!home)
		return;

	/* Scan Steam library cache for owned games */
	snprintf(path, sizeof(path), "%s/.local/share/Steam/appcache/librarycache", home);
	dir = opendir(path);
	if (!dir) {
		/* Try alternate path */
		snprintf(path, sizeof(path), "%s/.steam/steam/appcache/librarycache", home);
		dir = opendir(path);
	}
	if (!dir)
		return;

	while ((ent = readdir(dir)) != NULL) {
		/* Each directory is an app ID */
		if (ent->d_name[0] == '.')
			continue;

		/* Check if it's a valid number (app ID) */
		int valid = 1;
		for (int i = 0; ent->d_name[i]; i++) {
			if (ent->d_name[i] < '0' || ent->d_name[i] > '9') {
				valid = 0;
				break;
			}
		}
		if (!valid)
			continue;

		/* Only include games with library_600x900.jpg (portrait, 2:3 ratio) */
		snprintf(icon_path, sizeof(icon_path), "%s/%s/library_600x900.jpg", path, ent->d_name);
		if (access(icon_path, F_OK) != 0)
			continue;

		char launch[512];
		snprintf(launch, sizeof(launch), "steam steam://rungameid/%s", ent->d_name);

		/* Use app ID as name for now - Steam API can provide real names later */
		char name[256];
		snprintf(name, sizeof(name), "Steam Game %s", ent->d_name);

		/* acquired_time will be set from LastPlayed later, default to 0 */
		time_t acquired = 0;

		/* Store icon path in the game entry */
		GameEntry *g = calloc(1, sizeof(*g));
		if (!g)
			continue;

		snprintf(g->id, sizeof(g->id), "%s", ent->d_name);
		snprintf(g->name, sizeof(g->name), "%s", name);
		snprintf(g->icon_path, sizeof(g->icon_path), "%s", icon_path);
		snprintf(g->launch_cmd, sizeof(g->launch_cmd), "%s", launch);
		g->service = GAMING_SERVICE_STEAM;
		g->installed = 0;  /* In library but may not be installed */
		g->is_game = 1;    /* Assume game by default, API may set to 0 if DLC/tool */
		g->acquired_time = acquired;
		g->icon_buf = NULL;
		g->icon_loaded = 0;

		g->next = m->pc_gaming.games;
		m->pc_gaming.games = g;
		m->pc_gaming.game_count++;
	}
	closedir(dir);

	/* Also scan installed games from appmanifest files for names */
	const char *steam_paths[] = {
		"%s/.local/share/Steam/steamapps",
		"%s/.steam/steam/steamapps",
		NULL
	};

	for (int p = 0; steam_paths[p]; p++) {
		snprintf(path, sizeof(path), steam_paths[p], home);
		dir = opendir(path);
		if (!dir)
			continue;

		while ((ent = readdir(dir)) != NULL) {
			if (strncmp(ent->d_name, "appmanifest_", 12) != 0)
				continue;
			if (!strstr(ent->d_name, ".acf"))
				continue;

			char manifest_path[768];
			snprintf(manifest_path, sizeof(manifest_path), "%s/%s", path, ent->d_name);

			FILE *fp = fopen(manifest_path, "r");
			if (!fp)
				continue;

			char appid[32] = "";
			char name[256] = "";
			char line[1024];

			while (fgets(line, sizeof(line), fp)) {
				char *key, *val;

				key = strstr(line, "\"appid\"");
				if (key) {
					val = strchr(key + 7, '"');
					if (val) {
						val++;
						char *end = strchr(val, '"');
						if (end) {
							*end = '\0';
							snprintf(appid, sizeof(appid), "%s", val);
						}
					}
				}

				key = strstr(line, "\"name\"");
				if (key) {
					val = strchr(key + 6, '"');
					if (val) {
						val++;
						char *end = strchr(val, '"');
						if (end) {
							*end = '\0';
							snprintf(name, sizeof(name), "%s", val);
						}
					}
				}
			}
			fclose(fp);

			/* Update existing entry with real name and mark as installed */
			if (appid[0] && name[0]) {
				GameEntry *g = find_game_by_id(m->pc_gaming.games, appid, 0, 0);
				if (g) {
					snprintf(g->name, sizeof(g->name), "%s", name);
					g->installed = 1;
				}
			}
		}
		closedir(dir);
	}

	/* Fetch names and types from Steam API (single batch request) */
	pc_gaming_fetch_steam_names_batch(m);

	/* Mark installed games as actual games (appmanifest = real game, not DLC/tool) */
	GameEntry *ge = m->pc_gaming.games;
	while (ge) {
		if (ge->service == GAMING_SERVICE_STEAM && ge->installed)
			ge->is_game = 1;
		ge = ge->next;
	}

	/* Load playtime data */
	pc_gaming_load_steam_playtime(m);

	/* Filter out non-games */
	pc_gaming_filter_non_games(m);

	/* Sort by acquisition date */
	pc_gaming_sort_by_acquired(m);
}

void
pc_gaming_scan_heroic(Monitor *m)
{
	FILE *fp;
	char path[512];
	char *home;
	char *content = NULL;
	long fsize;
	char *p, *end;

	if (!m)
		return;

	home = getenv("HOME");
	if (!home)
		return;

	/* Parse Heroic library JSON files (Epic + GOG use same format) */
	{
		static const char *heroic_libs[] = {
			"legendary_library.json",
			"gog_library.json",
		};
		for (size_t li = 0; li < LENGTH(heroic_libs); li++) {
			size_t flen = 0;
			snprintf(path, sizeof(path), "%s/.config/heroic/store_cache/%s",
					home, heroic_libs[li]);
			content = read_file_to_string(path, &flen);
			if (!content || flen < 3 || flen > 10 * 1024 * 1024) {
				free(content);
				continue;
			}

			p = content;
			while ((p = strstr(p, "\"app_name\"")) != NULL) {
				char app_name[128] = "";
				char title[256] = "";

				char *val = strchr(p + 10, '"');
				if (val) {
					val++;
					end = strchr(val, '"');
					if (end) {
						size_t len = end - val;
						if (len < sizeof(app_name)) {
							memcpy(app_name, val, len);
							app_name[len] = '\0';
						}
					}
				}

				char *title_key = strstr(p, "\"title\"");
				if (title_key && title_key - p < 500) {
					val = strchr(title_key + 7, '"');
					if (val) {
						val++;
						end = strchr(val, '"');
						if (end) {
							size_t len = end - val;
							if (len < sizeof(title)) {
								memcpy(title, val, len);
								title[len] = '\0';
							}
						}
					}
				}

				if (app_name[0] && title[0]) {
					char launch[512];
					char icon[512] = "";
					snprintf(launch, sizeof(launch), "heroic --no-gui --launch %s", app_name);
					snprintf(icon, sizeof(icon), "%s/.config/heroic/images-cache/%s/art_square.jpg", home, app_name);
					if (access(icon, F_OK) != 0) {
						snprintf(icon, sizeof(icon), "%s/.config/heroic/images-cache/%s/logo.png", home, app_name);
						if (access(icon, F_OK) != 0)
							icon[0] = '\0';
					}
					pc_gaming_add_game(m, app_name, title, launch, icon, GAMING_SERVICE_HEROIC, 0);
				}

				p++;
			}
			free(content);
		}
	}
}

void
pc_gaming_refresh_games(Monitor *m)
{
	if (!m)
		return;

	pc_gaming_free_games(m);

	/* Try to load from cache first for fast startup */
	if (pc_gaming_load_cache(m)) {
		/* Always filter after loading - ensures name-based filtering works */
		pc_gaming_filter_non_games(m);
		pc_gaming_sort_by_acquired(m);
		m->pc_gaming.last_refresh_ms = monotonic_msec();
		m->pc_gaming.needs_refresh = 0;
		return;
	}

	/* Cache miss or stale - scan services */
	wlr_log(WLR_INFO, "Scanning game libraries...");

	/* Scan each enabled service */
	if (gaming_service_enabled[GAMING_SERVICE_STEAM])
		pc_gaming_scan_steam(m);
	if (gaming_service_enabled[GAMING_SERVICE_HEROIC])
		pc_gaming_scan_heroic(m);
	/* TODO: Add Lutris and Bottles scanning */

	/* Save to cache for next time */
	pc_gaming_save_cache(m);

	m->pc_gaming.last_refresh_ms = monotonic_msec();
	m->pc_gaming.needs_refresh = 0;
}

int
pc_gaming_cache_inotify_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	Monitor *m;
	int len;

	(void)mask;
	(void)data;

	/* Drain inotify events */
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		/* Just drain - we only care that something changed */
	}

	/* Mark all monitors as needing refresh */
	wl_list_for_each(m, &mons, link) {
		m->pc_gaming.needs_refresh = 1;
		/* If PC gaming view is visible, refresh immediately */
		if (m->pc_gaming.visible) {
			pc_gaming_refresh_games(m);
			pc_gaming_render(m);
		}
	}

	wlr_log(WLR_INFO, "Games cache updated, refreshing PC gaming view");
	return 0;
}

void
pc_gaming_cache_watch_setup(void)
{
	char cache_path[512];
	char cache_dir[512];
	char *home = getenv("HOME");

	if (!home)
		return;

	snprintf(cache_path, sizeof(cache_path), "%s" PC_GAMING_CACHE_FILE, home);
	snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/nixlytile", home);

	/* Create directory if it doesn't exist */
	mkdir(cache_dir, 0755);

	pc_gaming_cache_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (pc_gaming_cache_inotify_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to create inotify for games cache");
		return;
	}

	/* Watch the directory for new/modified files */
	pc_gaming_cache_inotify_wd = inotify_add_watch(pc_gaming_cache_inotify_fd,
		cache_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
	if (pc_gaming_cache_inotify_wd < 0) {
		wlr_log(WLR_ERROR, "Failed to add inotify watch for games cache");
		close(pc_gaming_cache_inotify_fd);
		pc_gaming_cache_inotify_fd = -1;
		return;
	}

	pc_gaming_cache_event = wl_event_loop_add_fd(wl_display_get_event_loop(dpy),
		pc_gaming_cache_inotify_fd, WL_EVENT_READABLE,
		pc_gaming_cache_inotify_cb, NULL);

	wlr_log(WLR_INFO, "Games cache inotify watch set up");
}

void
pc_gaming_ensure_visible(Monitor *m)
{
	PcGamingView *pg;
	int padding = PC_GAMING_PADDING;
	int gap = PC_GAMING_TILE_GAP;

	if (!m)
		return;

	pg = &m->pc_gaming;
	if (!pg->visible || pg->cols < 1)
		return;

	/* Calculate tile dimensions (5 columns) */
	int grid_width = pg->width - 2 * padding;
	int grid_height = pg->height - 2 * padding;
	int tile_w = (grid_width - (4 * gap)) / 5;
	int tile_h = (tile_w * 3) / 2;  /* Steam library images are 600x900 (2:3 ratio) */

	int selected_row = pg->selected_idx / pg->cols;
	int tile_top = selected_row * (tile_h + gap);
	int tile_bottom = tile_top + tile_h;

	/* Smooth scroll - adjust if selected tile is outside visible area */
	if (tile_top < pg->scroll_offset) {
		/* Tile is above visible area - scroll up */
		pg->scroll_offset = tile_top;
	} else if (tile_bottom > pg->scroll_offset + grid_height) {
		/* Tile is below visible area - scroll down */
		pg->scroll_offset = tile_bottom - grid_height;
	}

	/* Clamp scroll offset */
	int total_rows = (pg->game_count + pg->cols - 1) / pg->cols;
	int max_scroll = total_rows * (tile_h + gap) - grid_height;
	if (max_scroll < 0)
		max_scroll = 0;
	if (pg->scroll_offset < 0)
		pg->scroll_offset = 0;
	if (pg->scroll_offset > max_scroll)
		pg->scroll_offset = max_scroll;
}

void
pc_gaming_render(Monitor *m)
{
	PcGamingView *pg;
	struct wlr_scene_node *node, *tmp;
	int padding = PC_GAMING_PADDING;
	int gap = PC_GAMING_TILE_GAP;
	float tile_color[4] = {0.15f, 0.15f, 0.18f, 0.9f};
	float selected_border[4] = {0.3f, 0.6f, 1.0f, 1.0f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float name_bg[4] = {0.0f, 0.0f, 0.0f, 0.7f};

	if (!m || !statusfont.font)
		return;

	pg = &m->pc_gaming;
	if (!pg->visible || !pg->tree)
		return;

	/* Refresh games if needed */
	if (pg->needs_refresh || !pg->games) {
		pc_gaming_refresh_games(m);
	}

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &pg->tree->children, link) {
		wlr_scene_node_destroy(node);
	}
	pg->bg = NULL;
	pg->grid = NULL;
	pg->sidebar = NULL;

	/* Calculate grid dimensions */
	int grid_width = pg->width - 2 * padding;
	int grid_height = pg->height - 2 * padding;

	/* Fixed 5 columns - calculate tile size based on width */
	pg->cols = 5;
	int tile_w = (grid_width - ((pg->cols - 1) * gap)) / pg->cols;
	int tile_h = (tile_w * 3) / 2;  /* Steam library images are 600x900 (2:3 ratio) */
	int name_bar_h = 28;

	/* Ensure selected tile is visible */
	pc_gaming_ensure_visible(m);

	/* Create grid */
	pg->grid = wlr_scene_tree_create(pg->tree);
	if (!pg->grid)
		return;

	wlr_scene_node_set_position(&pg->grid->node, padding, padding);

	/* Draw game tiles */
	GameEntry *g = pg->games;
	int idx = 0;
	int visible_start = pg->scroll_offset / (tile_h + gap);
	int visible_rows = (grid_height + gap) / (tile_h + gap) + 2;

	while (g) {
		int row = idx / pg->cols;
		int col = idx % pg->cols;

		/* Only render visible tiles */
		if (row >= visible_start && row < visible_start + visible_rows) {
			int tx = col * (tile_w + gap);
			int ty = row * (tile_h + gap) - pg->scroll_offset;

			/* Skip if outside visible area */
			if (ty + tile_h > 0 && ty < grid_height) {
				int is_selected = (idx == pg->selected_idx);
				int glow_size = 8;  /* Glow/hover effect size */

				struct wlr_scene_tree *tile_tree = wlr_scene_tree_create(pg->grid);
				if (!tile_tree) {
					g = g->next;
					idx++;
					continue;
				}

				/* Position with offset for glow when selected */
				if (is_selected) {
					wlr_scene_node_set_position(&tile_tree->node, tx - glow_size, ty - glow_size);

					/* Draw outer glow layers (gradient effect) */
					float glow1[4] = {0.2f, 0.5f, 1.0f, 0.15f};  /* Outer glow */
					float glow2[4] = {0.3f, 0.6f, 1.0f, 0.25f};  /* Middle glow */
					float glow3[4] = {0.4f, 0.7f, 1.0f, 0.4f};   /* Inner glow */

					/* Outer glow */
					drawrect(tile_tree, 0, 0, tile_w + glow_size * 2, tile_h + glow_size * 2, glow1);
					/* Middle glow */
					drawrect(tile_tree, 2, 2, tile_w + glow_size * 2 - 4, tile_h + glow_size * 2 - 4, glow2);
					/* Inner glow */
					drawrect(tile_tree, 4, 4, tile_w + glow_size * 2 - 8, tile_h + glow_size * 2 - 8, glow3);

					/* Draw tile background inside glow */
					drawrect(tile_tree, glow_size, glow_size, tile_w, tile_h, tile_color);
				} else {
					wlr_scene_node_set_position(&tile_tree->node, tx, ty);
					/* Draw tile background */
					drawrect(tile_tree, 0, 0, tile_w, tile_h, tile_color);
				}

				int img_offset = is_selected ? glow_size : 0;

				/* Load and display game icon */
				if (!g->icon_loaded) {
					pc_gaming_load_game_icon(g, tile_w, tile_h - name_bar_h);
				}
				if (g->icon_buf) {
					struct wlr_scene_buffer *img = wlr_scene_buffer_create(tile_tree, g->icon_buf);
					if (img) {
						wlr_scene_node_set_position(&img->node, img_offset, img_offset);
					}
				}

				/* Installation progress bar */
				if (g->is_installing) {
					int bar_h = 6;
					int bar_y = img_offset + tile_h - name_bar_h - bar_h;
					int bar_w = tile_w;
					int progress_w = (bar_w * g->install_progress) / 100;

					/* Background bar (dark) */
					float bar_bg[4] = {0.1f, 0.1f, 0.1f, 0.9f};
					drawrect(tile_tree, img_offset, bar_y, bar_w, bar_h, bar_bg);

					/* Progress bar (blue gradient) */
					if (progress_w > 0) {
						float bar_fg[4] = {0.2f, 0.6f, 1.0f, 1.0f};
						drawrect(tile_tree, img_offset, bar_y, progress_w, bar_h, bar_fg);
					}

					/* Progress text */
					char progress_text[16];
					snprintf(progress_text, sizeof(progress_text), "%d%%", g->install_progress);
					int text_w = status_text_width(progress_text);
					struct wlr_scene_tree *progress_tree = wlr_scene_tree_create(tile_tree);
					if (progress_tree) {
						wlr_scene_node_set_position(&progress_tree->node,
							img_offset + (bar_w - text_w) / 2, bar_y - 2);
						StatusModule mod = {0};
						mod.tree = progress_tree;
						float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
						tray_render_label(&mod, progress_text, 0, 14, white);
					}
				}

				/* Selection highlight border */
				if (is_selected) {
					int bw = 3;
					float bright_border[4] = {0.4f, 0.75f, 1.0f, 1.0f};
					draw_border(tile_tree, glow_size, glow_size, tile_w, tile_h, bw, bright_border);
				}

				/* Name bar at bottom of tile */
				float active_name_bg[4] = {0.1f, 0.3f, 0.5f, 0.85f};  /* Highlighted name bar */
				drawrect(tile_tree, img_offset, img_offset + tile_h - name_bar_h, tile_w, name_bar_h,
				         is_selected ? active_name_bg : name_bg);

				/* Game name */
				struct wlr_scene_tree *name_tree = wlr_scene_tree_create(tile_tree);
				if (name_tree) {
					wlr_scene_node_set_position(&name_tree->node, img_offset, img_offset + tile_h - name_bar_h);
					StatusModule mod = {0};
					mod.tree = name_tree;

					/* Truncate name to fit */
					char short_name[64];
					int max_chars = (tile_w - 10) / 8;
					if (max_chars > (int)sizeof(short_name) - 1)
						max_chars = sizeof(short_name) - 1;
					if (max_chars < 1)
						max_chars = 1;
					strncpy(short_name, g->name, max_chars);
					short_name[max_chars] = '\0';
					if (strlen(g->name) > (size_t)max_chars && max_chars > 3) {
						short_name[max_chars - 1] = '.';
						short_name[max_chars - 2] = '.';
						short_name[max_chars - 3] = '.';
					}

					int label_w = status_text_width(short_name);
					int label_x = (tile_w - label_w) / 2;
					if (label_x < 4) label_x = 4;
					tray_render_label(&mod, short_name, label_x, name_bar_h,
					                  is_selected ? selected_border : text_color);
				}
			}
		}

		g = g->next;
		idx++;
	}

	/* Show "No games found" if empty */
	if (pg->game_count == 0) {
		struct wlr_scene_tree *empty_tree = wlr_scene_tree_create(pg->grid);
		if (empty_tree) {
			wlr_scene_node_set_position(&empty_tree->node, grid_width / 2 - 100, grid_height / 2);
			StatusModule mod = {0};
			mod.tree = empty_tree;
			tray_render_label(&mod, "No games found", 0, 30, text_color);
		}
	}
}

int
pc_gaming_install_timer_cb(void *data)
{
	Monitor *m;
	int any_installing = 0;

	(void)data;

	/* Find monitor with visible PC gaming view */
	m = pc_gaming_visible_monitor();
	if (!m || !m->pc_gaming.visible)
		return 0;

	/* Update installation status */
	pc_gaming_update_install_status(m);

	/* Check if any games are still installing */
	GameEntry *g = m->pc_gaming.games;
	while (g) {
		if (g->is_installing) {
			any_installing = 1;
			break;
		}
		g = g->next;
	}

	/* Re-render to show updated progress */
	pc_gaming_render(m);

	/* Reschedule timer if any games are installing */
	if (any_installing && pc_gaming_install_timer)
		wl_event_source_timer_update(pc_gaming_install_timer, 2000);

	return 0;
}

void
pc_gaming_start_install_timer(void)
{
	if (!event_loop)
		return;
	if (!pc_gaming_install_timer)
		pc_gaming_install_timer = wl_event_loop_add_timer(event_loop,
			pc_gaming_install_timer_cb, NULL);
	if (pc_gaming_install_timer)
		wl_event_source_timer_update(pc_gaming_install_timer, 2000);
}

void
pc_gaming_stop_install_timer(void)
{
	if (pc_gaming_install_timer)
		wl_event_source_timer_update(pc_gaming_install_timer, 0);
}

void
pc_gaming_show(Monitor *m)
{
	PcGamingView *pg;

	if (!m)
		return;

	pc_gaming_hide_all();
	gamepad_menu_hide_all();

	pg = &m->pc_gaming;
	pg->selected_idx = 0;
	pg->hover_idx = -1;
	pg->scroll_offset = 0;
	pg->service_filter = -1;
	pg->needs_refresh = 1;
	pg->view_tag = 1 << 3;  /* Tag 4 = bit 3 */

	/* Fullscreen on monitor - directly on background */
	pg->width = m->m.width;
	pg->height = m->m.height;
	pg->visible = 1;

	/* Use LyrBg+1 to be just above wallpaper but below windows */
	if (!pg->tree)
		pg->tree = wlr_scene_tree_create(layers[LyrBg]);
	if (!pg->tree)
		return;

	/* Position at monitor origin */
	wlr_scene_node_set_position(&pg->tree->node, m->m.x, m->m.y);
	wlr_scene_node_set_enabled(&pg->tree->node, 1);
	wlr_scene_node_raise_to_top(&pg->tree->node);

	/* Update install status and start timer */
	pc_gaming_update_install_status(m);
	pc_gaming_start_install_timer();

	pc_gaming_render(m);
}

void
pc_gaming_hide(Monitor *m)
{
	PcGamingView *pg;

	if (!m)
		return;

	pg = &m->pc_gaming;
	if (!pg->visible)
		return;

	pg->visible = 0;
	if (pg->tree)
		wlr_scene_node_set_enabled(&pg->tree->node, 0);

	/* Stop install progress timer */
	pc_gaming_stop_install_timer();
}

void
pc_gaming_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		pc_gaming_hide(m);
	}
}

Monitor *
pc_gaming_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->pc_gaming.visible)
			return m;
	}
	return NULL;
}

void
pc_gaming_launch_game(Monitor *m)
{
	PcGamingView *pg;
	GameEntry *g;
	int idx = 0;
	const char *gpu_params = NULL;
	GpuVendor gpu_vendor = GPU_VENDOR_UNKNOWN;
	char merged_options[1024];
	pid_t pid;

	if (!m)
		return;

	pg = &m->pc_gaming;
	if (!pg->visible || pg->selected_idx < 0)
		return;

	/* Don't launch if install popup is visible */
	if (pg->install_popup_visible)
		return;

	g = pg->games;
	while (g && idx < pg->selected_idx) {
		g = g->next;
		idx++;
	}

	if (!g || !g->launch_cmd[0])
		return;

	/* Check if game is installed */
	if (!g->installed) {
		/* Show install popup */
		pc_gaming_install_popup_show(m, g);
		return;
	}

	/* Get detected GPU vendor */
	if (discrete_gpu_idx >= 0 && discrete_gpu_idx < detected_gpu_count)
		gpu_vendor = detected_gpus[discrete_gpu_idx].vendor;

	/* Get GPU-specific launch params from hardcoded table */
	gpu_params = get_game_launch_params(g->id, gpu_vendor);

	/* Game is installed - launch it */
	wlr_log(WLR_INFO, "Launching game: %s (id=%s, GPU=%s, params=%s)", g->name, g->id,
		gpu_vendor == GPU_VENDOR_NVIDIA ? "NVIDIA" :
		gpu_vendor == GPU_VENDOR_AMD ? "AMD" :
		gpu_vendor == GPU_VENDOR_INTEL ? "Intel" : "Unknown",
		gpu_params ? gpu_params : "none");

	/* Fork and exec directly - spawn() doesn't work with struct member strings */
	pid = fork();
	if (pid == 0) {
		setsid();
		fork_detach();
		/* Set discrete GPU environment for gaming */
		set_dgpu_env();

		/* Set Steam env vars if this is a Steam game (uses steam:// protocol) */
		if (g->service == GAMING_SERVICE_STEAM || is_steam_cmd(g->launch_cmd)) {
			const char *user_options;

			set_steam_env();

			/* Get user's existing launch options (Steam stores them per-game)
			 * We can't read Steam's config here, but STEAM_GAME_LAUNCH_OPTIONS
			 * gets merged with user options by Steam itself.
			 * We build merged options that won't duplicate user settings. */

			/* For now, we don't have access to user's Steam launch options at runtime,
			 * but Steam will merge STEAM_GAME_LAUNCH_OPTIONS with user-set options.
			 * Our build_merged_launch_options ensures we don't add duplicates if
			 * user sets them in Steam. Steam applies user options AFTER env var. */

			user_options = getenv("STEAM_USER_LAUNCH_OPTIONS"); /* Future: read from Steam config */

			if (gpu_params && gpu_params[0]) {
				build_merged_launch_options(user_options, gpu_params,
					merged_options, sizeof(merged_options));
				setenv("STEAM_GAME_LAUNCH_OPTIONS", merged_options, 1);
				wlr_log(WLR_INFO, "Set STEAM_GAME_LAUNCH_OPTIONS=%s", merged_options);
			}

			/* If Steam isn't running, start it with GPU flags then run the game
			 * This ensures Steam UI has full GPU acceleration */
			if (!is_process_running("steam")) {
				char cmd[512];
				snprintf(cmd, sizeof(cmd),
					"steam -cef-force-gpu -cef-disable-sandbox %s",
					strstr(g->launch_cmd, "steam://") ? strstr(g->launch_cmd, "steam://") : "");
				wlr_log(WLR_INFO, "Starting Steam with GPU flags: %s", cmd);
				execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
				_exit(127);
			}
		} else if (gpu_params && gpu_params[0]) {
			/* Non-Steam game: prepend GPU params to command
			 * Replace %command% with actual game command, or prepend if no placeholder */
			char full_cmd[2048];
			const char *placeholder = strstr(gpu_params, "%command%");
			if (placeholder) {
				/* Replace %command% with the actual launch command */
				int prefix_len = placeholder - gpu_params;
				snprintf(full_cmd, sizeof(full_cmd), "%.*s%s%s",
					prefix_len, gpu_params,
					g->launch_cmd,
					placeholder + 9);  /* 9 = strlen("%command%") */
			} else {
				/* Just prepend params to command */
				snprintf(full_cmd, sizeof(full_cmd), "%s %s", gpu_params, g->launch_cmd);
			}
			wlr_log(WLR_INFO, "Launching with GPU params: %s", full_cmd);
			execl("/bin/sh", "sh", "-c", full_cmd, (char *)NULL);
			_exit(127);
		}

		execl("/bin/sh", "sh", "-c", g->launch_cmd, (char *)NULL);
		_exit(127);
	}
	pc_gaming_hide(m);
}

void
pc_gaming_install_popup_show(Monitor *m, GameEntry *g)
{
	PcGamingView *pg;
	float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.7f};

	if (!m || !g)
		return;

	pg = &m->pc_gaming;

	/* Store game info */
	snprintf(pg->install_game_id, sizeof(pg->install_game_id), "%s", g->id);
	snprintf(pg->install_game_name, sizeof(pg->install_game_name), "%s", g->name);
	pg->install_game_service = g->service;
	pg->install_popup_selected = 0;  /* Default to "Install" */
	pg->install_popup_visible = 1;

	/* Create dim overlay */
	if (!pg->install_dim)
		pg->install_dim = wlr_scene_tree_create(layers[LyrBlock]);
	if (pg->install_dim) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &pg->install_dim->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&pg->install_dim->node, m->m.x, m->m.y);
		drawrect(pg->install_dim, 0, 0, m->m.width, m->m.height, dim_color);
		wlr_scene_node_set_enabled(&pg->install_dim->node, 1);
	}

	/* Create popup */
	if (!pg->install_popup)
		pg->install_popup = wlr_scene_tree_create(layers[LyrBlock]);
	if (pg->install_popup) {
		wlr_scene_node_set_enabled(&pg->install_popup->node, 1);
		wlr_scene_node_raise_to_top(&pg->install_popup->node);
	}

	pc_gaming_install_popup_render(m);
}

void
pc_gaming_install_popup_hide(Monitor *m)
{
	PcGamingView *pg;

	if (!m)
		return;

	pg = &m->pc_gaming;
	pg->install_popup_visible = 0;

	if (pg->install_dim)
		wlr_scene_node_set_enabled(&pg->install_dim->node, 0);
	if (pg->install_popup)
		wlr_scene_node_set_enabled(&pg->install_popup->node, 0);
}

void
pc_gaming_install_popup_render(Monitor *m)
{
	PcGamingView *pg;
	struct wlr_scene_node *node, *tmp;
	int popup_w = 350, popup_h = 150;
	int x, y;
	float bg_color[4] = {0.12f, 0.12f, 0.14f, 0.98f};
	float border_color[4] = {0.35f, 0.35f, 0.4f, 1.0f};
	float button_color[4] = {0.18f, 0.18f, 0.22f, 1.0f};
	float selected_color[4] = {0.25f, 0.5f, 0.9f, 1.0f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	if (!m || !statusfont.font)
		return;

	pg = &m->pc_gaming;
	if (!pg->install_popup_visible || !pg->install_popup)
		return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &pg->install_popup->children, link)
		wlr_scene_node_destroy(node);

	/* Center popup on screen */
	x = m->m.x + (m->m.width - popup_w) / 2;
	y = m->m.y + (m->m.height - popup_h) / 2;
	wlr_scene_node_set_position(&pg->install_popup->node, x, y);

	/* Background */
	drawrect(pg->install_popup, 0, 0, popup_w, popup_h, bg_color);

	/* Border */
	draw_border(pg->install_popup, 0, 0, popup_w, popup_h, 2, border_color);

	/* Title - game name (truncated) */
	{
		char title[64];
		snprintf(title, sizeof(title), "%.40s", pg->install_game_name);
		if (strlen(pg->install_game_name) > 40)
			strcat(title, "...");
		int title_w = status_text_width(title);
		int title_x = (popup_w - title_w) / 2;
		struct wlr_scene_tree *title_tree = wlr_scene_tree_create(pg->install_popup);
		if (title_tree) {
			wlr_scene_node_set_position(&title_tree->node, title_x, 15);
			StatusModule mod = {0};
			mod.tree = title_tree;
			tray_render_label(&mod, title, 0, 30, text_color);
		}
	}

	/* "Not installed" text */
	{
		const char *msg = "Spillet er ikke installert";
		int msg_w = status_text_width(msg);
		int msg_x = (popup_w - msg_w) / 2;
		float gray[4] = {0.7f, 0.7f, 0.7f, 1.0f};
		struct wlr_scene_tree *msg_tree = wlr_scene_tree_create(pg->install_popup);
		if (msg_tree) {
			wlr_scene_node_set_position(&msg_tree->node, msg_x, 55);
			StatusModule mod = {0};
			mod.tree = msg_tree;
			tray_render_label(&mod, msg, 0, 25, gray);
		}
	}

	/* Buttons */
	int btn_w = 120, btn_h = 40;
	int btn_y = popup_h - 60;
	int install_x = popup_w / 2 - btn_w - 15;
	int close_x = popup_w / 2 + 15;

	/* Install button */
	drawrect(pg->install_popup, install_x, btn_y, btn_w, btn_h,
		pg->install_popup_selected == 0 ? selected_color : button_color);
	{
		const char *label = "Install";
		int label_w = status_text_width(label);
		struct wlr_scene_tree *btn_tree = wlr_scene_tree_create(pg->install_popup);
		if (btn_tree) {
			wlr_scene_node_set_position(&btn_tree->node, install_x + (btn_w - label_w) / 2, btn_y);
			StatusModule mod = {0};
			mod.tree = btn_tree;
			tray_render_label(&mod, label, 0, btn_h, text_color);
		}
	}

	/* Close button */
	drawrect(pg->install_popup, close_x, btn_y, btn_w, btn_h,
		pg->install_popup_selected == 1 ? selected_color : button_color);
	{
		const char *label = "Close";
		int label_w = status_text_width(label);
		struct wlr_scene_tree *btn_tree = wlr_scene_tree_create(pg->install_popup);
		if (btn_tree) {
			wlr_scene_node_set_position(&btn_tree->node, close_x + (btn_w - label_w) / 2, btn_y);
			StatusModule mod = {0};
			mod.tree = btn_tree;
			tray_render_label(&mod, label, 0, btn_h, text_color);
		}
	}
}

int
pc_gaming_install_popup_handle_button(Monitor *m, int button, int value)
{
	PcGamingView *pg;

	if (!m)
		return 0;

	pg = &m->pc_gaming;
	if (!pg->install_popup_visible)
		return 0;

	/* Consume all button releases when popup is open */
	if (value != 1)
		return 1;

	switch (button) {
	case BTN_DPAD_LEFT:
		if (pg->install_popup_selected > 0) {
			pg->install_popup_selected--;
			pc_gaming_install_popup_render(m);
		}
		return 1;
	case BTN_DPAD_RIGHT:
		if (pg->install_popup_selected < 1) {
			pg->install_popup_selected++;
			pc_gaming_install_popup_render(m);
		}
		return 1;
	case BTN_DPAD_UP:
	case BTN_DPAD_DOWN:
		/* Consume but ignore - only left/right changes selection */
		return 1;
	case BTN_SOUTH:  /* A button - confirm */
		if (pg->install_popup_selected == 0) {
			/* Install via the appropriate client */
			char cmd[1024];
			cmd[0] = '\0';
			switch (pg->install_game_service) {
			case GAMING_SERVICE_STEAM:
				/* Use Steam client directly - steamcmd installs to wrong dir and doesn't update manifests */
				snprintf(cmd, sizeof(cmd), "steam steam://install/%s", pg->install_game_id);
				break;
			case GAMING_SERVICE_HEROIC:
				snprintf(cmd, sizeof(cmd), "heroic heroic://install/%s", pg->install_game_id);
				break;
			case GAMING_SERVICE_LUTRIS:
				snprintf(cmd, sizeof(cmd), "lutris lutris:install/%s", pg->install_game_id);
				break;
			case GAMING_SERVICE_BOTTLES:
				snprintf(cmd, sizeof(cmd), "bottles");
				break;
			default:
				break;
			}
			if (cmd[0]) {
				wlr_log(WLR_INFO, "Installing game via %s: %s",
					gaming_service_names[pg->install_game_service], pg->install_game_name);
				/* Mark game as installing immediately so progress bar shows */
				GameEntry *g = find_game_by_id(pg->games, pg->install_game_id, 0, 0);
				if (g) {
					g->is_installing = 1;
					g->install_progress = 0;
				}
				/* Fork and exec directly - spawn() doesn't work with stack strings */
				pid_t pid = fork();
				if (pid == 0) {
					setsid();
					fork_detach();
					execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
					_exit(127);
				}
				/* Start/restart the install timer */
				pc_gaming_start_install_timer();
			}
		}
		pc_gaming_install_popup_hide(m);
		/* Re-render to show progress bar immediately */
		pc_gaming_render(m);
		return 1;
	case BTN_EAST:   /* B button - close */
		pc_gaming_install_popup_hide(m);
		return 1;
	default:
		/* Consume all other buttons when popup is open */
		return 1;
	}

	return 1;
}

void
pc_gaming_scroll(Monitor *m, int delta)
{
	PcGamingView *pg;
	int max_scroll;
	int rows;

	if (!m)
		return;

	pg = &m->pc_gaming;
	if (!pg->visible)
		return;

	rows = (pg->game_count + pg->cols - 1) / pg->cols;
	max_scroll = rows * (PC_GAMING_TILE_HEIGHT + PC_GAMING_TILE_GAP) - (pg->height - 40);
	if (max_scroll < 0)
		max_scroll = 0;

	pg->scroll_offset += delta;
	if (pg->scroll_offset < 0)
		pg->scroll_offset = 0;
	if (pg->scroll_offset > max_scroll)
		pg->scroll_offset = max_scroll;

	pc_gaming_render(m);
}

int
pc_gaming_handle_button(Monitor *m, int button, int value)
{
	PcGamingView *pg;
	Client *popup_client;

	if (!m)
		return 0;

	pg = &m->pc_gaming;
	/* Only handle input if view is visible AND on current tag */
	if (!htpc_view_is_active(m, pg->view_tag, pg->visible))
		return 0;

	/* If a window has focus (e.g. Steam dialog), click it and return to PC gaming */
	popup_client = focustop(m);
	if (popup_client && !popup_client->isfullscreen) {
		/* On A button press, click center of popup and dismiss it */
		if (value == 1 && button == BTN_SOUTH) {
			double cx = popup_client->geom.x + popup_client->geom.width / 2.0;
			double cy = popup_client->geom.y + popup_client->geom.height / 2.0;
			uint32_t time_msec = (uint32_t)(monotonic_msec() & 0xFFFFFFFF);
			wlr_cursor_warp(cursor, NULL, cx, cy);
			/* Update pointer focus and send click */
			struct wlr_surface *surface = client_surface(popup_client);
			if (surface) {
				double sx = popup_client->geom.width / 2.0;
				double sy = popup_client->geom.height / 2.0;
				wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
				wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
				/* Send click (press + release) */
				wlr_seat_pointer_notify_button(seat, time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
				wlr_seat_pointer_notify_button(seat, time_msec + 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			}
			/* Unfocus popup and return to PC gaming */
			focusclient(NULL, 0);
			wlr_seat_pointer_notify_clear_focus(seat);
			return 1;
		}
		/* On B button, just dismiss popup without clicking */
		if (value == 1 && button == BTN_EAST) {
			focusclient(NULL, 0);
			wlr_seat_pointer_notify_clear_focus(seat);
			return 1;
		}
		return 0;
	}

	/* Handle install popup first if visible */
	if (pg->install_popup_visible)
		return pc_gaming_install_popup_handle_button(m, button, value);

	/* Only handle button press */
	if (value != 1)
		return 0;

	switch (button) {
	case BTN_SOUTH:  /* A button - launch game */
		pc_gaming_launch_game(m);
		return 1;
	case BTN_EAST:   /* B button - close PC gaming */
		pc_gaming_hide(m);
		return 1;
	/* BTN_MODE (guide) handled in gamepad_menu_handle_button to show menu overlay */
	case BTN_DPAD_UP:
		if (pg->selected_idx >= pg->cols) {
			pg->selected_idx -= pg->cols;
			pc_gaming_render(m);
		}
		return 1;
	case BTN_DPAD_DOWN:
		if (pg->selected_idx + pg->cols < pg->game_count) {
			pg->selected_idx += pg->cols;
			pc_gaming_render(m);
		}
		return 1;
	case BTN_DPAD_LEFT:
		if (pg->selected_idx > 0) {
			pg->selected_idx--;
			pc_gaming_render(m);
		}
		return 1;
	case BTN_DPAD_RIGHT:
		if (pg->selected_idx < pg->game_count - 1) {
			pg->selected_idx++;
			pc_gaming_render(m);
		}
		return 1;
	case BTN_TL:  /* Left bumper - scroll up */
		pc_gaming_scroll(m, -(PC_GAMING_TILE_HEIGHT + PC_GAMING_TILE_GAP) * 3);
		return 1;
	case BTN_TR:  /* Right bumper - scroll down */
		pc_gaming_scroll(m, (PC_GAMING_TILE_HEIGHT + PC_GAMING_TILE_GAP) * 3);
		return 1;
	}

	return 0;
}

int
pc_gaming_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	PcGamingView *pg;

	if (!m)
		return 0;

	pg = &m->pc_gaming;
	if (!pg->visible)
		return 0;

	switch (sym) {
	case XKB_KEY_Escape:
		pc_gaming_hide(m);
		return 1;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		pc_gaming_launch_game(m);
		return 1;
	case XKB_KEY_Up:
	case XKB_KEY_k:
		if (pg->selected_idx >= pg->cols) {
			pg->selected_idx -= pg->cols;
			pc_gaming_render(m);
		}
		return 1;
	case XKB_KEY_Down:
	case XKB_KEY_j:
		if (pg->selected_idx + pg->cols < pg->game_count) {
			pg->selected_idx += pg->cols;
			pc_gaming_render(m);
		}
		return 1;
	case XKB_KEY_Left:
	case XKB_KEY_h:
		if (pg->selected_idx > 0) {
			pg->selected_idx--;
			pc_gaming_render(m);
		}
		return 1;
	case XKB_KEY_Right:
	case XKB_KEY_l:
		if (pg->selected_idx < pg->game_count - 1) {
			pg->selected_idx++;
			pc_gaming_render(m);
		}
		return 1;
	case XKB_KEY_Page_Up:
		pc_gaming_scroll(m, -(pg->height - 100));
		return 1;
	case XKB_KEY_Page_Down:
		pc_gaming_scroll(m, pg->height - 100);
		return 1;
	case XKB_KEY_Home:
		pg->scroll_offset = 0;
		pg->selected_idx = 0;
		pc_gaming_render(m);
		return 1;
	case XKB_KEY_End:
		if (pg->game_count > 0) {
			pg->selected_idx = pg->game_count - 1;
			int rows = (pg->game_count + pg->cols - 1) / pg->cols;
			int max_scroll = rows * (PC_GAMING_TILE_HEIGHT + PC_GAMING_TILE_GAP) - (pg->height - 40);
			if (max_scroll > 0)
				pg->scroll_offset = max_scroll;
			pc_gaming_render(m);
		}
		return 1;
	}

	return 1;  /* Consume all keys while view is open */
}

/* ==========================================================================
 * Retro Gaming - Game List, Cover Art, and Emulator Launch
 * ========================================================================== */

static int
retro_parse_roms_json(const char *buffer, const char *server_url,
                      RomItem *items, int max_items)
{
	const char *p = buffer;
	int idx = 0;

	while (idx < max_items) {
		const char *obj_start = strchr(p, '{');
		if (!obj_start) break;

		int depth = 1;
		const char *obj_end = obj_start + 1;
		while (*obj_end && depth > 0) {
			if (*obj_end == '{') depth++;
			else if (*obj_end == '}') depth--;
			obj_end++;
		}
		if (depth != 0) break;

		size_t obj_len = obj_end - obj_start;
		char *obj = malloc(obj_len + 1);
		if (!obj) break;
		memcpy(obj, obj_start, obj_len);
		obj[obj_len] = '\0';

		items[idx].id = json_extract_int(obj, "id");
		items[idx].console = json_extract_int(obj, "console");
		json_extract_string(obj, "title", items[idx].title, sizeof(items[idx].title));
		json_extract_string(obj, "cover", items[idx].cover_path, sizeof(items[idx].cover_path));
		json_extract_string(obj, "filepath", items[idx].filepath, sizeof(items[idx].filepath));
		json_extract_string(obj, "description", items[idx].description, sizeof(items[idx].description));
		json_extract_string(obj, "developer", items[idx].developer, sizeof(items[idx].developer));
		json_extract_string(obj, "publisher", items[idx].publisher, sizeof(items[idx].publisher));
		items[idx].release_year = json_extract_int(obj, "release_year");
		json_extract_string(obj, "genre", items[idx].genre, sizeof(items[idx].genre));
		json_extract_string(obj, "igdb_platforms", items[idx].igdb_platforms, sizeof(items[idx].igdb_platforms));
		items[idx].rating = json_extract_float(obj, "igdb_rating");
		items[idx].igdb_id = json_extract_int(obj, "igdb_id");
		json_extract_string(obj, "emulator", items[idx].emulator, sizeof(items[idx].emulator));
		items[idx].size = json_extract_int64(obj, "size");
		strncpy(items[idx].server_url, server_url, sizeof(items[idx].server_url) - 1);

		free(obj);
		p = obj_end;
		idx++;
	}

	return idx;
}

static void
retro_gaming_build_active_consoles(RetroGamingView *rg)
{
	int has_roms[RETRO_CONSOLE_COUNT] = {0};
	int i;

	for (i = 0; i < rg->all_rom_count; i++)
		if (rg->all_roms[i].console >= 0 && rg->all_roms[i].console < RETRO_CONSOLE_COUNT
		    && rg->all_roms[i].igdb_id > 0)
			has_roms[rg->all_roms[i].console] = 1;

	rg->active_console_count = 0;
	for (i = 0; i < RETRO_CONSOLE_COUNT; i++)
		if (has_roms[i])
			rg->active_consoles[rg->active_console_count++] = i;

	rg->selected_console = 0;
	rg->target_console = 0;
}

static int
rom_cmp_rating(const void *a, const void *b)
{
	const RomItem *ra = a, *rb = b;
	/* Descending by rating */
	if (rb->rating > ra->rating) return 1;
	if (rb->rating < ra->rating) return -1;
	return strcasecmp(ra->title, rb->title);
}

static int
rom_cmp_alpha(const void *a, const void *b)
{
	const RomItem *ra = a, *rb = b;
	return strcasecmp(ra->title, rb->title);
}

static void
retro_gaming_filter_games(Monitor *m)
{
	RetroGamingView *rg;
	int real_console, i, count;

	if (!m)
		return;

	rg = &m->retro_gaming;

	/* Free old filtered list */
	if (rg->games) {
		free(rg->games);
		rg->games = NULL;
	}
	rg->game_count = 0;
	rg->selected_game = 0;
	rg->game_scroll_offset = 0;
	if (rg->cover_buf) {
		wlr_buffer_drop(rg->cover_buf);
		rg->cover_buf = NULL;
	}
	rg->cover_loaded = 0;
	rg->cover_loading_idx = -1;

	if (rg->active_console_count == 0 || rg->all_rom_count == 0)
		return;

	real_console = rg->active_consoles[rg->selected_console];

	/* Count matching scraped ROMs */
	count = 0;
	for (i = 0; i < rg->all_rom_count; i++)
		if (rg->all_roms[i].console == real_console && rg->all_roms[i].igdb_id > 0)
			count++;

	if (count == 0)
		return;

	rg->games = calloc(count, sizeof(RomItem));
	if (!rg->games)
		return;

	rg->game_count = 0;
	for (i = 0; i < rg->all_rom_count; i++) {
		if (rg->all_roms[i].console == real_console && rg->all_roms[i].igdb_id > 0) {
			rg->games[rg->game_count] = rg->all_roms[i];
			rg->game_count++;
		}
	}

	/* Sort by current mode */
	if (rg->game_count > 1) {
		if (rg->sort_mode == 1)
			qsort(rg->games, rg->game_count, sizeof(RomItem), rom_cmp_alpha);
		else
			qsort(rg->games, rg->game_count, sizeof(RomItem), rom_cmp_rating);
	}
}

void
retro_gaming_fetch_all_roms(Monitor *m)
{
	RetroGamingView *rg;
	FILE *fp;
	char cmd[512];
	size_t bytes_read;

	if (!m)
		return;

	rg = &m->retro_gaming;

	/* Free old master list */
	if (rg->all_roms) {
		free(rg->all_roms);
		rg->all_roms = NULL;
	}
	rg->all_rom_count = 0;

	/* 2MB buffer for ROM JSON (collections can be 1MB+) */
	#define ROM_BUFFER_SIZE (2 * 1024 * 1024)
	char *buffer = malloc(ROM_BUFFER_SIZE);
	if (!buffer) return;

	/* Temporary buffer for all ROMs from all servers */
	#define MAX_TEMP_ROMS 8000
	RomItem *temp_roms = calloc(MAX_TEMP_ROMS, sizeof(RomItem));
	if (!temp_roms) { free(buffer); return; }
	int total_roms = 0;

	/* Fetch from all configured/discovered servers (same pattern as media) */
	MediaServer *servers;
	int server_count = get_all_media_servers(&servers);

	if (server_count == 0) {
		/* Fallback to localhost */
		snprintf(cmd, sizeof(cmd),
			"curl -s --connect-timeout 2 'http://localhost:8080/api/roms' 2>/dev/null");
		fp = popen(cmd, "r");
		if (fp) {
			bytes_read = fread(buffer, 1, ROM_BUFFER_SIZE - 1, fp);
			pclose(fp);
			if (bytes_read > 0) {
				buffer[bytes_read] = '\0';
				total_roms = retro_parse_roms_json(buffer, "http://localhost:8080",
					temp_roms, MAX_TEMP_ROMS);
			}
		}
	} else {
		for (int s = 0; s < server_count && total_roms < MAX_TEMP_ROMS; s++) {
			snprintf(cmd, sizeof(cmd),
				"curl -s --connect-timeout 2 '%s/api/roms' 2>/dev/null",
				servers[s].url);
			fp = popen(cmd, "r");
			if (!fp) continue;

			bytes_read = fread(buffer, 1, ROM_BUFFER_SIZE - 1, fp);
			pclose(fp);
			if (bytes_read == 0) continue;
			buffer[bytes_read] = '\0';

			int parsed = retro_parse_roms_json(buffer, servers[s].url,
				&temp_roms[total_roms], MAX_TEMP_ROMS - total_roms);
			total_roms += parsed;
		}
	}

	free(buffer);

	if (total_roms == 0) {
		free(temp_roms);
		return;
	}

	/* Move to final allocation */
	rg->all_roms = calloc(total_roms, sizeof(RomItem));
	if (!rg->all_roms) {
		free(temp_roms);
		return;
	}
	memcpy(rg->all_roms, temp_roms, total_roms * sizeof(RomItem));
	rg->all_rom_count = total_roms;
	free(temp_roms);

	/* Build active console tabs and filter for initial console */
	retro_gaming_build_active_consoles(rg);
	retro_gaming_filter_games(m);
}

static void
retro_gaming_load_cover(Monitor *m)
{
	RetroGamingView *rg;
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	char local_path[512];
	struct stat st;

	if (!m)
		return;

	rg = &m->retro_gaming;

	/* Already loaded for this game */
	if (rg->cover_loaded && rg->cover_loading_idx == rg->selected_game)
		return;

	/* Drop old cover */
	if (rg->cover_buf) {
		wlr_buffer_drop(rg->cover_buf);
		rg->cover_buf = NULL;
	}
	rg->cover_loaded = 1;
	rg->cover_loading_idx = rg->selected_game;
	rg->cover_w = 0;
	rg->cover_h = 0;

	if (rg->selected_game >= rg->game_count)
		return;

	RomItem *game = &rg->games[rg->selected_game];
	if (game->cover_path[0] == '\0' || strcmp(game->cover_path, "null") == 0)
		return;

	/* Check if the cover is a local path or needs download from server */
	const char *cover = game->cover_path;
	const char *fname = strrchr(cover, '/');
	fname = fname ? fname + 1 : cover;

	/* Build local cache path */
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";
	char cache_dir[512];
	snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/nixlytile/covers", home);
	snprintf(local_path, sizeof(local_path), "%s/%s", cache_dir, fname);

	/* Check if cached locally */
	if (stat(local_path, &st) != 0 || st.st_size == 0) {
		/* Try to download from server */
		char cmd[1024];
		const char *srv = game->server_url[0] ? game->server_url : get_media_server_url();
		snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && curl -s -f -o '%s' '%s/image/%s' 2>/dev/null",
			cache_dir, local_path, srv, fname);
		system(cmd);

		if (stat(local_path, &st) != 0 || st.st_size == 0)
			return;
	}

	pixbuf = gdk_pixbuf_new_from_file(local_path, &gerr);
	if (!pixbuf) {
		if (gerr) g_error_free(gerr);
		return;
	}

	/* Scale to fit the cover area (right side, max 55% height for metadata below) */
	int content_h = rg->height - 80;
	int target_h = (content_h * 55) / 100;
	int orig_w = gdk_pixbuf_get_width(pixbuf);
	int orig_h = gdk_pixbuf_get_height(pixbuf);
	double scale = (double)target_h / orig_h;
	int max_w = (rg->width * 80) / 100;  /* Up to 80% of right panel */
	if ((int)(orig_w * scale) > max_w)
		scale = (double)max_w / orig_w;

	int scaled_w = (int)(orig_w * scale);
	int scaled_h = (int)(orig_h * scale);

	GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, scaled_w, scaled_h, GDK_INTERP_BILINEAR);
	g_object_unref(pixbuf);

	if (!scaled)
		return;

	rg->cover_buf = statusbar_buffer_from_pixbuf(scaled, scaled_h, &rg->cover_w, &rg->cover_h);
	g_object_unref(scaled);
}

void
retro_gaming_render_game_list(Monitor *m)
{
	RetroGamingView *rg;
	int menu_bar_h = 80;
	int padding = 20;
	int item_h = 50;
	int accent_bar_w = 3;
	int list_w, list_x, cover_x, cover_w;
	int content_h;
	int visible_items;
	float list_bg[4] = {0.08f, 0.08f, 0.10f, 1.0f};
	float sel_bg[4] = {0.14f, 0.18f, 0.26f, 1.0f};
	float sel_accent[4] = {0.30f, 0.55f, 0.95f, 1.0f};
	float alt_bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};
	float scrollbar_bg[4] = {0.15f, 0.15f, 0.18f, 1.0f};
	float scrollbar_fg[4] = {0.35f, 0.35f, 0.40f, 1.0f};

	if (!m)
		return;

	rg = &m->retro_gaming;

	/* Layout: 40% left = game list, 60% right = cover art */
	list_w = (rg->width * 40) / 100;
	list_x = 0;
	cover_x = list_w;
	cover_w = rg->width - list_w;
	content_h = rg->height - menu_bar_h;
	visible_items = content_h / item_h;

	/* Ensure scroll keeps selected item visible */
	if (rg->selected_game < rg->game_scroll_offset)
		rg->game_scroll_offset = rg->selected_game;
	if (rg->selected_game >= rg->game_scroll_offset + visible_items)
		rg->game_scroll_offset = rg->selected_game - visible_items + 1;

	/* List background */
	drawrect(rg->tree, list_x, menu_bar_h, list_w, content_h, list_bg);

	/* Game count label (top-right of list area) */
	if (rg->game_count > 0) {
		struct wlr_scene_tree *cnt_tree = wlr_scene_tree_create(rg->tree);
		if (cnt_tree) {
			char count_str[32];
			snprintf(count_str, sizeof(count_str), "%d games", rg->game_count);
			float count_color[4] = {0.5f, 0.5f, 0.5f, 0.7f};
			/* Position at top-right of list, inside menu bar area */
			int cnt_x = list_w - padding - (int)strlen(count_str) * 8;
			if (cnt_x < padding) cnt_x = padding;
			wlr_scene_node_set_position(&cnt_tree->node, cnt_x, menu_bar_h - 24);
			StatusModule mod = {0};
			mod.tree = cnt_tree;
			tray_render_label(&mod, count_str, 0, 14, count_color);
		}
	}

	/* Render visible game items */
	for (int i = 0; i < visible_items && (i + rg->game_scroll_offset) < rg->game_count; i++) {
		int game_idx = i + rg->game_scroll_offset;
		int y = menu_bar_h + i * item_h;
		int is_selected = (game_idx == rg->selected_game);

		if (is_selected) {
			/* Selected: dark blue-tinted background + accent bar */
			drawrect(rg->tree, list_x, y, list_w - 8, item_h, sel_bg);
			drawrect(rg->tree, list_x, y, accent_bar_w, item_h, sel_accent);
		} else if (game_idx % 2 == 1) {
			/* Alternating subtle tint for odd rows */
			drawrect(rg->tree, list_x, y, list_w - 8, item_h, alt_bg);
		}

		/* Game title text */
		struct wlr_scene_tree *text_tree = wlr_scene_tree_create(rg->tree);
		if (text_tree) {
			int text_indent = is_selected ? (accent_bar_w + 8 + padding) : padding;
			wlr_scene_node_set_position(&text_tree->node, list_x + text_indent, y + (item_h - 20) / 2);
			StatusModule mod = {0};
			mod.tree = text_tree;
			float text_color[4] = {1.0f, 1.0f, 1.0f, is_selected ? 1.0f : 0.6f};
			/* Truncate title to fit */
			char display_title[64];
			int max_chars = (list_w - text_indent - padding - 8) / 10;
			if (max_chars > (int)sizeof(display_title) - 1)
				max_chars = sizeof(display_title) - 1;
			if (max_chars < 1) max_chars = 1;
			strncpy(display_title, rg->games[game_idx].title, max_chars);
			display_title[max_chars] = '\0';
			tray_render_label(&mod, display_title, 0, 20, text_color);
		}
	}

	/* Scrollbar */
	if (rg->game_count > visible_items) {
		int sb_x = list_x + list_w - 6;
		int sb_h = content_h;
		drawrect(rg->tree, sb_x, menu_bar_h, 4, sb_h, scrollbar_bg);

		/* Scrollbar thumb */
		int thumb_h = (visible_items * sb_h) / rg->game_count;
		if (thumb_h < 20) thumb_h = 20;
		int thumb_y = menu_bar_h + (rg->game_scroll_offset * (sb_h - thumb_h)) /
			(rg->game_count - visible_items);
		drawrect(rg->tree, sb_x, thumb_y, 4, thumb_h, scrollbar_fg);
	}

	/* Detail panel area (right side) */
	float cover_bg[4] = {0.05f, 0.05f, 0.07f, 1.0f};
	drawrect(rg->tree, cover_x, menu_bar_h, cover_w, content_h, cover_bg);

	if (rg->game_count <= 0)
		return;

	RomItem *game = &rg->games[rg->selected_game];
	int detail_pad = 24;
	int text_x = cover_x + detail_pad;
	int text_w = cover_w - detail_pad * 2;
	int cur_y = menu_bar_h + detail_pad;

	/* Load and render cover art (top of panel, centered) */
	retro_gaming_load_cover(m);

	if (rg->cover_buf && rg->cover_w > 0 && rg->cover_h > 0) {
		int cx = cover_x + (cover_w - rg->cover_w) / 2;
		struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(rg->tree, rg->cover_buf);
		if (sbuf)
			wlr_scene_node_set_position(&sbuf->node, cx, cur_y);
		cur_y += rg->cover_h + detail_pad;

		/* Thin separator line between cover art and metadata */
		float sep_color[4] = {0.25f, 0.25f, 0.30f, 0.6f};
		drawrect(rg->tree, text_x, cur_y, text_w, 1, sep_color);
		cur_y += detail_pad / 2;
	}

	/* --- Metadata text below cover --- */
	float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float grey[4] = {0.7f, 0.7f, 0.7f, 1.0f};
	float accent[4] = {0.4f, 0.7f, 1.0f, 1.0f};
	int line_h = 28;

	/* Title + Year (larger 28px) */
	{
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
		if (t) {
			wlr_scene_node_set_position(&t->node, text_x, cur_y);
			StatusModule mod = {0};
			mod.tree = t;
			char title_line[320];
			if (game->release_year > 0)
				snprintf(title_line, sizeof(title_line), "%s  (%d)", game->title, game->release_year);
			else
				snprintf(title_line, sizeof(title_line), "%s", game->title);
			int max_chars = text_w / 13;
			if (max_chars > 0 && max_chars < (int)sizeof(title_line))
				title_line[max_chars] = '\0';
			tray_render_label(&mod, title_line, 0, 28, white);
		}
		cur_y += line_h + 8;
	}

	/* Developer / Publisher */
	if (game->developer[0] || game->publisher[0]) {
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
		if (t) {
			wlr_scene_node_set_position(&t->node, text_x, cur_y);
			StatusModule mod = {0};
			mod.tree = t;
			char company_line[320];
			if (game->developer[0] && game->publisher[0] &&
			    strcmp(game->developer, game->publisher) != 0)
				snprintf(company_line, sizeof(company_line), "%s / %s",
					game->developer, game->publisher);
			else if (game->developer[0])
				snprintf(company_line, sizeof(company_line), "%s", game->developer);
			else
				snprintf(company_line, sizeof(company_line), "%s", game->publisher);
			tray_render_label(&mod, company_line, 0, 18, accent);
		}
		cur_y += line_h;
	}

	/* Genre */
	if (game->genre[0]) {
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
		if (t) {
			wlr_scene_node_set_position(&t->node, text_x, cur_y);
			StatusModule mod = {0};
			mod.tree = t;
			tray_render_label(&mod, game->genre, 0, 18, grey);
		}
		cur_y += line_h;
	}

	/* Platforms */
	if (game->igdb_platforms[0]) {
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
		if (t) {
			wlr_scene_node_set_position(&t->node, text_x, cur_y);
			StatusModule mod = {0};
			mod.tree = t;
			char plat_line[560];
			snprintf(plat_line, sizeof(plat_line), "Platforms: %s", game->igdb_platforms);
			int max_chars = text_w / 10;
			if (max_chars > 0 && max_chars < (int)sizeof(plat_line))
				plat_line[max_chars] = '\0';
			tray_render_label(&mod, plat_line, 0, 18, grey);
		}
		cur_y += line_h;
	}

	/* Rating: filled bar + numeric value */
	if (game->rating > 0) {
		int bar_w = 100;
		int bar_h = 8;
		int bar_y_offset = 6;
		/* Rating bar background (dark) */
		float bar_bg[4] = {0.20f, 0.20f, 0.25f, 1.0f};
		drawrect(rg->tree, text_x, cur_y + bar_y_offset, bar_w, bar_h, bar_bg);
		/* Rating bar fill */
		int fill_w = (int)(bar_w * game->rating / 100.0f);
		if (fill_w > bar_w) fill_w = bar_w;
		float bar_fill[4] = {0.30f, 0.55f, 0.95f, 1.0f};
		if (fill_w > 0)
			drawrect(rg->tree, text_x, cur_y + bar_y_offset, fill_w, bar_h, bar_fill);
		/* Numeric value next to bar */
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
		if (t) {
			wlr_scene_node_set_position(&t->node, text_x + bar_w + 10, cur_y);
			StatusModule mod = {0};
			mod.tree = t;
			char rating_str[16];
			snprintf(rating_str, sizeof(rating_str), "%.0f", game->rating);
			tray_render_label(&mod, rating_str, 0, 18, accent);
		}
		cur_y += line_h + 8;
	}

	/* Description (multi-line, word-wrapped, max 10 lines) */
	if (game->description[0]) {
		float desc_color[4] = {0.8f, 0.8f, 0.8f, 0.9f};
		int desc_font = 16;
		int char_w = desc_font * 6 / 10;
		int chars_per_line = (char_w > 0) ? (text_w / char_w) : 60;
		if (chars_per_line < 20) chars_per_line = 20;
		if (chars_per_line > 120) chars_per_line = 120;
		int max_lines = (menu_bar_h + content_h - cur_y - detail_pad) / (desc_font + 6);
		if (max_lines < 1) max_lines = 1;
		if (max_lines > 10) max_lines = 10;

		const char *src = game->description;
		for (int line = 0; line < max_lines && *src; line++) {
			char line_buf[256];
			int len = 0;
			int last_space = -1;

			while (src[len] && len < chars_per_line && len < (int)sizeof(line_buf) - 1) {
				if (src[len] == '\n') { len++; break; }
				if (src[len] == ' ') last_space = len;
				len++;
			}

			if (src[len] && src[len] != ' ' && src[len] != '\n' && last_space > 0) {
				len = last_space + 1;
			}

			memcpy(line_buf, src, len);
			line_buf[len] = '\0';

			while (len > 0 && (line_buf[len - 1] == ' ' || line_buf[len - 1] == '\n'))
				line_buf[--len] = '\0';

			if (line_buf[0]) {
				struct wlr_scene_tree *t = wlr_scene_tree_create(rg->tree);
				if (t) {
					wlr_scene_node_set_position(&t->node, text_x, cur_y);
					StatusModule mod = {0};
					mod.tree = t;
					tray_render_label(&mod, line_buf, 0, desc_font, desc_color);
				}
				cur_y += desc_font + 6;
			}

			src += len;
			while (*src == ' ') src++;
		}
	}
}

static void
retro_launch_rom_local(Monitor *m, const char *local_path, const char *emulator_tag,
                       int console)
{
	const char *cmd_fmt;
	char cmd[2048];
	pid_t pid;

	/* Try user-configured emulator first, fall back to hardcoded */
	cmd_fmt = retro_get_emulator_cmd(emulator_tag);
	if (!cmd_fmt && console >= 0 && console < RETRO_CONSOLE_COUNT)
		cmd_fmt = retro_emulator_cmds[console];
	if (!cmd_fmt) {
		wlr_log(WLR_ERROR, "No emulator configured for tag '%s'", emulator_tag);
		return;
	}

	snprintf(cmd, sizeof(cmd), cmd_fmt, local_path);
	wlr_log(WLR_INFO, "Launching ROM: %s", cmd);

	pid = fork();
	if (pid == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		fork_detach();
		if (console == RETRO_N64 || console == RETRO_GAMECUBE || console == RETRO_WII) {
			if (should_use_dgpu(cmd))
				set_dgpu_env();
		}
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		_exit(127);
	}
}

static void
retro_download_render_overlay(Monitor *m)
{
	RetroGamingView *rg = &m->retro_gaming;
	struct wlr_scene_node *node, *tmp;
	int popup_w = 400, popup_h = 160;
	int x, y;
	float bg_color[4] = {0.12f, 0.12f, 0.14f, 0.98f};
	float border_color[4] = {0.35f, 0.35f, 0.4f, 1.0f};
	float bar_bg[4] = {0.2f, 0.2f, 0.25f, 1.0f};
	float bar_fill[4] = {0.30f, 0.55f, 0.95f, 1.0f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float gray[4] = {0.7f, 0.7f, 0.7f, 1.0f};

	if (!rg->download_overlay || !statusfont.font)
		return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &rg->download_overlay->children, link)
		wlr_scene_node_destroy(node);

	/* Center popup on screen */
	x = m->m.x + (m->m.width - popup_w) / 2;
	y = m->m.y + (m->m.height - popup_h) / 2;
	wlr_scene_node_set_position(&rg->download_overlay->node, x, y);

	/* Background + border */
	drawrect(rg->download_overlay, 0, 0, popup_w, popup_h, bg_color);
	draw_border(rg->download_overlay, 0, 0, popup_w, popup_h, 2, border_color);

	/* Title (game name, centered) */
	if (rg->download_title[0]) {
		char truncated[64];
		snprintf(truncated, sizeof(truncated), "%s", rg->download_title);
		int title_w = status_text_width(truncated);
		int title_x = (popup_w - title_w) / 2;
		if (title_x < 10) title_x = 10;
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->download_overlay);
		if (t) {
			wlr_scene_node_set_position(&t->node, title_x, 15);
			StatusModule mod = {0};
			mod.tree = t;
			tray_render_label(&mod, truncated, 0, 30, text_color);
		}
	}

	/* "Downloading..." label */
	{
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->download_overlay);
		if (t) {
			const char *msg = "Downloading...";
			int msg_w = status_text_width(msg);
			int msg_x = (popup_w - msg_w) / 2;
			wlr_scene_node_set_position(&t->node, msg_x, 50);
			StatusModule mod = {0};
			mod.tree = t;
			tray_render_label(&mod, msg, 0, 25, gray);
		}
	}

	/* Progress bar */
	int bar_x = 50, bar_y = 85, bar_w = 300, bar_h = 12;
	float progress = 0.0f;
	if (rg->download_total > 0) {
		struct stat st;
		if (stat(rg->download_path, &st) == 0)
			progress = (float)st.st_size / (float)rg->download_total;
	}
	if (progress > 1.0f) progress = 1.0f;

	drawrect(rg->download_overlay, bar_x, bar_y, bar_w, bar_h, bar_bg);
	if (progress > 0.0f)
		drawrect(rg->download_overlay, bar_x, bar_y,
			(int)(bar_w * progress), bar_h, bar_fill);

	/* Percentage + speed text */
	{
		char info[64];
		int pct = (int)(progress * 100.0f);
		if (rg->download_speed_mbps > 0.1f)
			snprintf(info, sizeof(info), "%d%% — %.1f MB/s", pct, rg->download_speed_mbps);
		else
			snprintf(info, sizeof(info), "%d%%", pct);
		int info_w = status_text_width(info);
		int info_x = (popup_w - info_w) / 2;
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->download_overlay);
		if (t) {
			wlr_scene_node_set_position(&t->node, info_x, bar_y + bar_h + 10);
			StatusModule mod = {0};
			mod.tree = t;
			tray_render_label(&mod, info, 0, 25, gray);
		}
	}

	/* Cancel hint */
	{
		struct wlr_scene_tree *t = wlr_scene_tree_create(rg->download_overlay);
		if (t) {
			const char *hint = "Press B to cancel";
			int hw = status_text_width(hint);
			wlr_scene_node_set_position(&t->node, (popup_w - hw) / 2, popup_h - 28);
			StatusModule mod = {0};
			mod.tree = t;
			float dim[4] = {0.5f, 0.5f, 0.5f, 1.0f};
			tray_render_label(&mod, hint, 0, 20, dim);
		}
	}

	wlr_scene_node_set_enabled(&rg->download_overlay->node, 1);
}

static int
retro_download_progress_cb(void *data)
{
	Monitor *m = data;
	RetroGamingView *rg = &m->retro_gaming;

	if (!rg->download_active)
		return 0;

	struct stat st;
	int64_t current_bytes = 0;
	if (stat(rg->download_path, &st) == 0)
		current_bytes = st.st_size;

	/* Calculate speed */
	uint64_t now = monotonic_msec();
	uint64_t elapsed = now - rg->download_last_check_ms;
	if (elapsed > 0 && rg->download_last_check_ms > 0) {
		int64_t delta = current_bytes - rg->download_last_bytes;
		rg->download_speed_mbps = (float)delta / (float)elapsed * 1000.0f / (1024.0f * 1024.0f);
	}
	rg->download_last_check_ms = now;
	rg->download_last_bytes = current_bytes;

	/* Check if curl child finished */
	int status;
	pid_t ret = waitpid(rg->download_pid, &status, WNOHANG);
	if (ret > 0) {
		/* Child finished */
		rg->download_active = 0;
		rg->download_pid = 0;

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			/* Download complete — hide overlay and launch */
			if (rg->download_overlay)
				wlr_scene_node_set_enabled(&rg->download_overlay->node, 0);
			wlr_log(WLR_INFO, "ROM download complete: %s", rg->download_path);
			retro_launch_rom_local(m, rg->download_path,
				rg->download_emulator, rg->download_console);
		} else {
			/* Download failed — hide overlay, delete partial */
			wlr_log(WLR_ERROR, "ROM download failed (exit %d)",
				WIFEXITED(status) ? WEXITSTATUS(status) : -1);
			unlink(rg->download_path);
			if (rg->download_overlay)
				wlr_scene_node_set_enabled(&rg->download_overlay->node, 0);
		}
		return 0;
	}

	/* Still downloading — re-render overlay and reschedule */
	retro_download_render_overlay(m);
	if (rg->download_timer)
		wl_event_source_timer_update(rg->download_timer, 200);
	return 0;
}

void
retro_download_cancel(Monitor *m)
{
	RetroGamingView *rg;

	if (!m)
		return;
	rg = &m->retro_gaming;
	if (!rg->download_active)
		return;

	if (rg->download_pid > 0) {
		kill(rg->download_pid, SIGTERM);
		waitpid(rg->download_pid, NULL, 0);
		rg->download_pid = 0;
	}
	rg->download_active = 0;
	unlink(rg->download_path);
	if (rg->download_overlay)
		wlr_scene_node_set_enabled(&rg->download_overlay->node, 0);
}

void
retro_gaming_launch_game(Monitor *m)
{
	RetroGamingView *rg;

	if (!m)
		return;

	rg = &m->retro_gaming;
	if (rg->selected_game >= rg->game_count || !rg->games)
		return;
	if (rg->download_active)
		return;

	RomItem *game = &rg->games[rg->selected_game];
	int console = game->console;
	const char *emu_tag = game->emulator;

	/* Build local path: ~/.local/nixly/roms/{emulator}/{filename} */
	char *home = getenv("HOME");
	if (!home) home = "/tmp";

	/* Sanitize emulator tag — alphanumeric only, no path traversal */
	char safe_tag[32];
	{
		const char *s = emu_tag;
		int i = 0;
		while (*s && i < (int)sizeof(safe_tag) - 1) {
			if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))
				safe_tag[i++] = *s;
			s++;
		}
		safe_tag[i] = '\0';
		if (!safe_tag[0])
			snprintf(safe_tag, sizeof(safe_tag), "unknown");
	}

	/* Extract filename from server filepath (basename only) */
	const char *filename = strrchr(game->filepath, '/');
	filename = filename ? filename + 1 : game->filepath;
	if (!filename[0] || strchr(filename, '/')) {
		wlr_log(WLR_ERROR, "Invalid ROM filename");
		return;
	}

	char local_dir[512];
	char local_path[512];
	snprintf(local_dir, sizeof(local_dir), "%s/.local/nixly/roms/%s", home, safe_tag);
	snprintf(local_path, sizeof(local_path), "%s/%s", local_dir, filename);

	/* Check if already cached locally (require exact size match) */
	struct stat st;
	if (game->size > 0 && stat(local_path, &st) == 0 &&
	    st.st_size == game->size) {
		wlr_log(WLR_INFO, "ROM cached locally: %s", local_path);
		retro_launch_rom_local(m, local_path, emu_tag, console);
		return;
	}

	/* Need to download — build URL */
	const char *srv = game->server_url[0] ? game->server_url : get_media_server_url();
	if (!srv || !srv[0]) {
		wlr_log(WLR_ERROR, "No server URL for ROM download");
		return;
	}

	char url[1024];
	snprintf(url, sizeof(url), "%s/api/roms/%d/download", srv, game->id);

	/* Create directory (recursive, no shell) */
	{
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s", local_dir);
		for (char *p = tmp + 1; *p; p++) {
			if (*p == '/') {
				*p = '\0';
				mkdir(tmp, 0755);
				*p = '/';
			}
		}
		mkdir(tmp, 0755);
	}

	/* Store download state */
	snprintf(rg->download_path, sizeof(rg->download_path), "%s", local_path);
	snprintf(rg->download_title, sizeof(rg->download_title), "%s", game->title);
	snprintf(rg->download_emulator, sizeof(rg->download_emulator), "%s", emu_tag);
	rg->download_console = console;
	rg->download_rom_id = game->id;
	rg->download_total = game->size;
	rg->download_last_check_ms = monotonic_msec();
	rg->download_last_bytes = 0;
	rg->download_speed_mbps = 0.0f;

	/* Fork curl to download — delete any partial leftover first */
	unlink(local_path);

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		fork_detach();
		execlp("curl", "curl", "-s", "-f", "-o", local_path, url, NULL);
		_exit(127);
	}
	if (pid < 0) {
		wlr_log(WLR_ERROR, "Failed to fork curl for ROM download");
		return;
	}

	rg->download_pid = pid;
	rg->download_active = 1;

	/* Create overlay tree if needed */
	if (!rg->download_overlay && rg->tree)
		rg->download_overlay = wlr_scene_tree_create(rg->tree);

	/* Render initial overlay */
	retro_download_render_overlay(m);

	/* Start progress timer (200ms interval) */
	if (!rg->download_timer)
		rg->download_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy),
			retro_download_progress_cb, m);
	if (rg->download_timer)
		wl_event_source_timer_update(rg->download_timer, 200);

	wlr_log(WLR_INFO, "Downloading ROM: %s → %s", url, local_path);
}

void
retro_gaming_show(Monitor *m)
{
	RetroGamingView *rg;
	float dim_color[4] = {0.05f, 0.05f, 0.08f, 0.98f};

	if (!m)
		return;

	retro_gaming_hide_all();

	rg = &m->retro_gaming;
	rg->selected_console = 0;
	rg->target_console = 0;
	rg->anim_direction = 0;
	rg->slide_offset = 0.0f;
	rg->width = m->m.width;
	rg->height = m->m.height;
	rg->visible = 1;
	rg->view_tag = 1 << 2;  /* Tag 3 = bit 2 */
	rg->game_count = 0;
	rg->selected_game = 0;
	rg->game_scroll_offset = 0;
	if (rg->games) { free(rg->games); rg->games = NULL; }
	if (rg->cover_buf) { wlr_buffer_drop(rg->cover_buf); rg->cover_buf = NULL; }
	rg->cover_loaded = 0;
	rg->cover_loading_idx = -1;
	rg->sort_mode = 0;  /* Default: sort by rating */

	/* Fetch all ROMs once and build dynamic console tabs */
	rg->in_game_list = 1;
	retro_gaming_fetch_all_roms(m);

	/* Create dim overlay */
	if (!rg->dim)
		rg->dim = wlr_scene_tree_create(layers[LyrBlock]);
	if (rg->dim) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &rg->dim->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&rg->dim->node, m->m.x, m->m.y);
		drawrect(rg->dim, 0, 0, m->m.width, m->m.height, dim_color);
		wlr_scene_node_set_enabled(&rg->dim->node, 1);
		wlr_scene_node_raise_to_top(&rg->dim->node);
	}

	/* Create main tree */
	if (!rg->tree)
		rg->tree = wlr_scene_tree_create(layers[LyrBlock]);
	if (rg->tree) {
		wlr_scene_node_set_position(&rg->tree->node, m->m.x, m->m.y);
		wlr_scene_node_set_enabled(&rg->tree->node, 1);
		wlr_scene_node_raise_to_top(&rg->tree->node);
	}

	retro_gaming_render(m);

	/* Hide mouse cursor in retro gaming browser */
	wlr_cursor_unset_image(cursor);
}

void
retro_gaming_hide(Monitor *m)
{
	RetroGamingView *rg;

	if (!m)
		return;

	rg = &m->retro_gaming;

	/* Cancel any active download */
	retro_download_cancel(m);

	rg->visible = 0;
	rg->in_game_list = 0;
	retro_dpad_repeat_stop();

	if (rg->games) { free(rg->games); rg->games = NULL; }
	rg->game_count = 0;
	if (rg->all_roms) { free(rg->all_roms); rg->all_roms = NULL; }
	rg->all_rom_count = 0;
	rg->active_console_count = 0;
	if (rg->cover_buf) { wlr_buffer_drop(rg->cover_buf); rg->cover_buf = NULL; }
	rg->cover_loaded = 0;

	if (rg->tree)
		wlr_scene_node_set_enabled(&rg->tree->node, 0);
	if (rg->dim)
		wlr_scene_node_set_enabled(&rg->dim->node, 0);

	/* Restore mouse cursor when leaving retro gaming browser */
	if (!retro_gaming_visible_monitor())
		nixly_cursor_set_xcursor("default");
}

void
retro_gaming_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		retro_gaming_hide(m);
	}
}

void
retro_gaming_render(Monitor *m)
{
	RetroGamingView *rg;
	struct wlr_scene_node *node, *tmp;
	int menu_bar_h = 80;
	int item_spacing = 40;
	int total_width = 0;
	int x_offset;
	float bg_color[4] = {0.08f, 0.08f, 0.10f, 1.0f};
	float selected_color[4] = {0.20f, 0.45f, 0.85f, 1.0f};
	float hover_underline[4] = {0.20f, 0.45f, 0.85f, 1.0f};

	if (!m || !statusfont.font)
		return;

	rg = &m->retro_gaming;
	if (!rg->visible || !rg->tree)
		return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &rg->tree->children, link) {
		wlr_scene_node_destroy(node);
	}

	/* Draw menu bar background */
	drawrect(rg->tree, 0, 0, rg->width, menu_bar_h, bg_color);

	/* Calculate total width and positions of menu items */
	int tab_count = rg->active_console_count > 0 ? rg->active_console_count : 0;
	int item_positions[RETRO_CONSOLE_COUNT];
	int item_widths[RETRO_CONSOLE_COUNT];
	for (int i = 0; i < tab_count; i++) {
		item_widths[i] = status_text_width(retro_console_names[rg->active_consoles[i]]);
		total_width += item_widths[i];
		if (i < tab_count - 1)
			total_width += item_spacing;
	}

	/* Calculate item positions (menu items stay fixed) */
	x_offset = (rg->width - total_width) / 2;
	for (int i = 0; i < tab_count; i++) {
		item_positions[i] = x_offset;
		x_offset += item_widths[i] + item_spacing;
	}

	/* Calculate animated pill position */
	int pill_pad = 15;
	int pill_h = 40;
	int pill_y = (menu_bar_h - pill_h) / 2;

	/* Interpolate pill position during animation */
	int from_console = rg->selected_console;
	int to_console = rg->target_console;
	float anim_progress = 1.0f - fabsf(rg->slide_offset);  /* slide_offset goes from +-1 to 0 */

	int pill_x, pill_w;
	if (from_console == to_console || anim_progress >= 1.0f) {
		/* No animation or animation complete */
		pill_x = item_positions[rg->selected_console] - pill_pad;
		pill_w = item_widths[rg->selected_console] + pill_pad * 2;
	} else {
		/* Animate pill between positions */
		int from_x = item_positions[from_console] - pill_pad;
		int from_w = item_widths[from_console] + pill_pad * 2;
		int to_x = item_positions[to_console] - pill_pad;
		int to_w = item_widths[to_console] + pill_pad * 2;

		/* Ease-out interpolation */
		float ease = 1.0f - (1.0f - anim_progress) * (1.0f - anim_progress);
		pill_x = from_x + (int)((to_x - from_x) * ease);
		pill_w = from_w + (int)((to_w - from_w) * ease);
	}

	/* Draw selection pill (animated) */
	drawrect(rg->tree, pill_x, pill_y, pill_w, pill_h, selected_color);

	/* Draw menu items (fixed positions) */
	for (int i = 0; i < tab_count; i++) {
		const char *name = retro_console_names[rg->active_consoles[i]];
		int text_y = (menu_bar_h - 20) / 2;
		int is_target = (i == rg->target_console);

		/* Draw text */
		struct wlr_scene_tree *text_tree = wlr_scene_tree_create(rg->tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, item_positions[i], text_y);
			StatusModule mod = {0};
			mod.tree = text_tree;
			float text_color[4] = {1.0f, 1.0f, 1.0f, is_target ? 1.0f : 0.7f};
			tray_render_label(&mod, name, 0, 20, text_color);
		}
	}

	/* Draw underline (animated with pill) */
	int underline_x = pill_x + pill_pad;
	int underline_w = pill_w - pill_pad * 2;
	drawrect(rg->tree, underline_x, menu_bar_h - 4, underline_w, 3, hover_underline);

	/* Draw separator line */
	float sep_color[4] = {0.3f, 0.3f, 0.35f, 1.0f};
	drawrect(rg->tree, 0, menu_bar_h - 1, rg->width, 1, sep_color);

	/* Content area */
	float content_bg[4] = {0.06f, 0.06f, 0.08f, 1.0f};
	drawrect(rg->tree, 0, menu_bar_h, rg->width, rg->height - menu_bar_h, content_bg);

	if (rg->in_game_list && rg->game_count > 0) {
		/* Show game list with cover art */
		retro_gaming_render_game_list(m);
	} else if (rg->in_game_list && rg->game_count == 0) {
		/* No games found for this console */
		struct wlr_scene_tree *content_tree = wlr_scene_tree_create(rg->tree);
		if (content_tree) {
			const char *msg = "No games found";
			int cw = status_text_width(msg);
			int cx = (rg->width - cw) / 2;
			int cy = menu_bar_h + (rg->height - menu_bar_h) / 2 - 20;
			wlr_scene_node_set_position(&content_tree->node, cx, cy);
			StatusModule mod = {0};
			mod.tree = content_tree;
			float grey[4] = {0.5f, 0.5f, 0.5f, 0.7f};
			tray_render_label(&mod, msg, 0, 40, grey);
		}
	} else {
		/* Show target console name in content area (shows destination immediately) */
		struct wlr_scene_tree *content_tree = wlr_scene_tree_create(rg->tree);
		if (content_tree) {
			const char *console = tab_count > 0
				? retro_console_names[rg->active_consoles[rg->target_console]]
				: "No ROMs";
			int cw = status_text_width(console);
			int cx = (rg->width - cw) / 2;
			int cy = menu_bar_h + (rg->height - menu_bar_h) / 2 - 20;
			wlr_scene_node_set_position(&content_tree->node, cx, cy);
			StatusModule mod = {0};
			mod.tree = content_tree;
			float white[4] = {1.0f, 1.0f, 1.0f, 0.5f};
			tray_render_label(&mod, console, 0, 40, white);
		}
	}
}

int
retro_gaming_animate(void *data)
{
	Monitor *m = data;
	RetroGamingView *rg;
	uint64_t now, elapsed;
	float progress;

	if (!m)
		return 0;

	rg = &m->retro_gaming;
	if (!rg->visible)
		return 0;

	now = monotonic_msec();
	elapsed = now - rg->slide_start_ms;
	progress = (float)elapsed / RETRO_SLIDE_DURATION_MS;

	if (progress >= 1.0f) {
		/* Animation complete */
		rg->selected_console = rg->target_console;
		rg->slide_offset = 0.0f;
		rg->anim_direction = 0;
		/* Filter games locally for the newly selected console */
		rg->in_game_list = 1;
		retro_gaming_filter_games(m);
		retro_gaming_render(m);
		return 0;
	}

	/* Ease-out animation - slide_offset goes from 1.0 to 0.0 */
	float ease = 1.0f - (1.0f - progress) * (1.0f - progress);
	rg->slide_offset = (1.0f - ease) * (rg->anim_direction < 0 ? 1.0f : -1.0f);

	retro_gaming_render(m);

	/* Continue animation */
	if (retro_anim_timer)
		wl_event_source_timer_update(retro_anim_timer, 16);  /* ~60fps */

	return 0;
}

/* ── D-pad hold-to-repeat for retro gaming list ─────────────────── */
static struct wl_event_source *retro_dpad_repeat_timer;
static int retro_dpad_held_button;
static Monitor *retro_dpad_held_mon;

static int
retro_dpad_repeat_cb(void *data)
{
	(void)data;

	if (!retro_dpad_held_button || !retro_dpad_held_mon ||
	    !retro_dpad_held_mon->retro_gaming.visible) {
		retro_dpad_held_button = 0;
		retro_dpad_held_mon = NULL;
		return 0;
	}

	RetroGamingView *rg = &retro_dpad_held_mon->retro_gaming;
	int old_sel = rg->selected_game;

	retro_gaming_handle_button(retro_dpad_held_mon, retro_dpad_held_button, 1);

	if (rg->selected_game != old_sel) {
		if (retro_dpad_repeat_timer)
			wl_event_source_timer_update(retro_dpad_repeat_timer, RETRO_DPAD_REPEAT_RATE);
	} else {
		retro_dpad_held_button = 0;
		retro_dpad_held_mon = NULL;
	}

	return 0;
}

void
retro_dpad_repeat_start(Monitor *m, int button)
{
	if (!m || !m->retro_gaming.visible)
		return;

	retro_dpad_held_button = button;
	retro_dpad_held_mon = m;

	if (!retro_dpad_repeat_timer) {
		retro_dpad_repeat_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), retro_dpad_repeat_cb, NULL);
	}

	if (retro_dpad_repeat_timer)
		wl_event_source_timer_update(retro_dpad_repeat_timer, RETRO_DPAD_INITIAL_DELAY);
}

void
retro_dpad_repeat_stop(void)
{
	retro_dpad_held_button = 0;
	retro_dpad_held_mon = NULL;
	if (retro_dpad_repeat_timer)
		wl_event_source_timer_update(retro_dpad_repeat_timer, 0);
}

int
retro_gaming_handle_button(Monitor *m, int button, int value)
{
	RetroGamingView *rg;
	int direction = 0;
	int content_h, item_h, visible_items, page_size;

	if (!m)
		return 0;

	rg = &m->retro_gaming;
	/* Only handle input if view is visible AND on current tag */
	if (!htpc_view_is_active(m, rg->view_tag, rg->visible))
		return 0;

	/* Only handle button press */
	if (value != 1)
		return 0;

	if (rg->in_game_list) {
		/* === Game list mode (with integrated console switching) === */
		item_h = 50;
		content_h = rg->height - 80;
		visible_items = content_h / item_h;
		page_size = visible_items > 2 ? visible_items - 2 : 1;

		switch (button) {
		case BTN_DPAD_UP:
			if (rg->selected_game > 0) {
				rg->selected_game--;
				rg->cover_loaded = 0;  /* Force cover reload */
				retro_gaming_render(m);
			}
			return 1;
		case BTN_DPAD_DOWN:
			if (rg->selected_game < rg->game_count - 1) {
				rg->selected_game++;
				rg->cover_loaded = 0;  /* Force cover reload */
				retro_gaming_render(m);
			}
			return 1;
		case BTN_TL: {  /* LB = jump to previous letter */
			/* Switch to alphabetical sort if needed */
			if (rg->sort_mode != 1) {
				char prev_title[256];
				strncpy(prev_title, rg->games[rg->selected_game].title, sizeof(prev_title) - 1);
				prev_title[sizeof(prev_title) - 1] = '\0';
				rg->sort_mode = 1;
				qsort(rg->games, rg->game_count, sizeof(RomItem), rom_cmp_alpha);
				/* Find closest match to previously selected game */
				for (int k = 0; k < rg->game_count; k++) {
					if (strcasecmp(rg->games[k].title, prev_title) >= 0) {
						rg->selected_game = k;
						break;
					}
				}
			}
			int cur = rg->selected_game;
			char cur_letter = toupper(rg->games[cur].title[0]);
			/* Find first game with a letter before current */
			int j = cur - 1;
			while (j >= 0 && toupper(rg->games[j].title[0]) == cur_letter)
				j--;
			if (j >= 0) {
				/* Found a game with different letter, find first of that letter */
				char prev_letter = toupper(rg->games[j].title[0]);
				while (j > 0 && toupper(rg->games[j - 1].title[0]) == prev_letter)
					j--;
				rg->selected_game = j;
			} else {
				rg->selected_game = 0;
			}
			rg->cover_loaded = 0;
			retro_gaming_render(m);
			return 1;
		}
		case BTN_TR: {  /* RB = jump to next letter */
			/* Switch to alphabetical sort if needed */
			if (rg->sort_mode != 1) {
				char prev_title[256];
				strncpy(prev_title, rg->games[rg->selected_game].title, sizeof(prev_title) - 1);
				prev_title[sizeof(prev_title) - 1] = '\0';
				rg->sort_mode = 1;
				qsort(rg->games, rg->game_count, sizeof(RomItem), rom_cmp_alpha);
				for (int k = 0; k < rg->game_count; k++) {
					if (strcasecmp(rg->games[k].title, prev_title) >= 0) {
						rg->selected_game = k;
						break;
					}
				}
			}
			int cur = rg->selected_game;
			char cur_letter = toupper(rg->games[cur].title[0]);
			/* Find first game with a letter after current */
			int j = cur + 1;
			while (j < rg->game_count && toupper(rg->games[j].title[0]) == cur_letter)
				j++;
			if (j < rg->game_count)
				rg->selected_game = j;
			rg->cover_loaded = 0;
			retro_gaming_render(m);
			return 1;
		}
		case BTN_SOUTH:  /* A button - launch game */
			retro_gaming_launch_game(m);
			return 1;
		case BTN_DPAD_LEFT: {  /* Switch to previous console */
			int new_target = rg->target_console - 1;
			if (new_target >= 0) {
				if (rg->anim_direction != 0)
					rg->selected_console = rg->target_console;
				rg->target_console = new_target;
				rg->anim_direction = -1;
				rg->slide_start_ms = monotonic_msec();
				rg->slide_offset = 1.0f;
				if (!retro_anim_timer)
					retro_anim_timer = wl_event_loop_add_timer(
						wl_display_get_event_loop(dpy),
						retro_gaming_animate, m);
				if (retro_anim_timer)
					wl_event_source_timer_update(retro_anim_timer, 16);
			}
			return 1;
		}
		case BTN_DPAD_RIGHT: {  /* Switch to next console */
			int new_target = rg->target_console + 1;
			if (new_target < rg->active_console_count) {
				if (rg->anim_direction != 0)
					rg->selected_console = rg->target_console;
				rg->target_console = new_target;
				rg->anim_direction = 1;
				rg->slide_start_ms = monotonic_msec();
				rg->slide_offset = -1.0f;
				if (!retro_anim_timer)
					retro_anim_timer = wl_event_loop_add_timer(
						wl_display_get_event_loop(dpy),
						retro_gaming_animate, m);
				if (retro_anim_timer)
					wl_event_source_timer_update(retro_anim_timer, 16);
			}
			return 1;
		}
		case BTN_EAST:  /* B button - cancel download or exit */
			if (rg->download_active) {
				retro_download_cancel(m);
				return 1;
			}
			retro_gaming_hide(m);
			return 1;
		case BTN_MODE:  /* Guide button - let main handler show menu overlay */
			return 0;
		default:
			return 1;
		}
	}

	/* === Console selector mode (fallback) === */
	switch (button) {
	case BTN_DPAD_LEFT:
	case BTN_TL:  /* Left shoulder */
		direction = -1;
		break;
	case BTN_DPAD_RIGHT:
	case BTN_TR:  /* Right shoulder */
		direction = 1;
		break;
	case BTN_EAST:  /* B button - exit retro gaming */
		retro_gaming_hide(m);
		return 1;
	case BTN_MODE:  /* Guide button - let main handler show menu overlay */
		return 0;
	case BTN_SOUTH:  /* A button - enter game list and launch */
		rg->selected_console = rg->target_console;
		rg->in_game_list = 1;
		retro_gaming_filter_games(m);
		retro_gaming_render(m);
		return 1;
	}

	if (direction != 0) {
		int new_target = rg->target_console + direction;
		if (new_target >= 0 && new_target < rg->active_console_count) {
			/* If animation is in progress, update selected to current target first */
			if (rg->anim_direction != 0) {
				rg->selected_console = rg->target_console;
			}
			rg->target_console = new_target;
			rg->anim_direction = direction;
			rg->slide_start_ms = monotonic_msec();
			rg->slide_offset = (direction < 0) ? 1.0f : -1.0f;

			/* Start animation timer */
			if (!retro_anim_timer)
				retro_anim_timer = wl_event_loop_add_timer(
					wl_display_get_event_loop(dpy),
					retro_gaming_animate, m);
			if (retro_anim_timer)
				wl_event_source_timer_update(retro_anim_timer, 16);
		}
		return 1;
	}

	return 0;
}

Monitor *
retro_gaming_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->retro_gaming.visible)
			return m;
	}
	return NULL;
}

/* Check if a kernel module is loaded via /sys/module/<name> */
static int
validate_kernel_module(const char *module_name)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/module/%s", module_name);
	return access(path, F_OK) == 0;
}

/* Check if a kernel module parameter has a specific value */
static int
check_module_param(const char *module_name, const char *param, const char *expected)
{
	char path[256], val[64];
	snprintf(path, sizeof(path), "/sys/module/%s/parameters/%s", module_name, param);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, val, sizeof(val) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	val[n] = '\0';
	while (n > 0 && (val[n-1] == '\n' || val[n-1] == ' '))
		val[--n] = '\0';
	return strcmp(val, expected) == 0;
}

void
detect_gpus(void)
{
	DIR *dri_dir;
	struct dirent *ent;
	char path[256], link_target[256], driver_link[256];
	ssize_t len;
	int i;

	detected_gpu_count = 0;
	discrete_gpu_idx = -1;
	integrated_gpu_idx = -1;

	dri_dir = opendir("/sys/class/drm");
	if (!dri_dir) {
		wlr_log(WLR_ERROR, "Failed to open /sys/class/drm for GPU detection");
		return;
	}

	while ((ent = readdir(dri_dir)) != NULL && detected_gpu_count < MAX_GPUS) {
		int card_num;
		GpuInfo *gpu;

		/* Only process cardX entries (not renderDX or other entries) */
		if (sscanf(ent->d_name, "card%d", &card_num) != 1)
			continue;
		/* Skip card0-HDMI-A-1 style entries */
		if (strchr(ent->d_name, '-'))
			continue;

		gpu = &detected_gpus[detected_gpu_count];
		memset(gpu, 0, sizeof(*gpu));
		gpu->card_index = card_num;

		snprintf(gpu->card_path, sizeof(gpu->card_path), "/dev/dri/card%d", card_num);

		/* Find corresponding render node */
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device", card_num);
		DIR *dev_dir = opendir(path);
		if (dev_dir) {
			struct dirent *dev_ent;
			while ((dev_ent = readdir(dev_dir)) != NULL) {
				int render_num;
				if (sscanf(dev_ent->d_name, "drm/renderD%d", &render_num) == 1 ||
				    (strncmp(dev_ent->d_name, "drm", 3) == 0)) {
					/* Check for renderDXXX in drm subdirectory */
					char drm_path[300];
					snprintf(drm_path, sizeof(drm_path), "%s/drm", path);
					DIR *drm_dir = opendir(drm_path);
					if (drm_dir) {
						struct dirent *drm_ent;
						while ((drm_ent = readdir(drm_dir)) != NULL) {
							if (sscanf(drm_ent->d_name, "renderD%d", &render_num) == 1) {
								gpu->render_index = render_num;
								snprintf(gpu->render_path, sizeof(gpu->render_path),
									"/dev/dri/renderD%d", render_num);
								break;
							}
						}
						closedir(drm_dir);
					}
					break;
				}
			}
			closedir(dev_dir);
		}

		/* Get driver name from symlink */
		snprintf(driver_link, sizeof(driver_link),
			"/sys/class/drm/card%d/device/driver", card_num);
		len = readlink(driver_link, link_target, sizeof(link_target) - 1);
		if (len > 0) {
			link_target[len] = '\0';
			const char *driver_name = strrchr(link_target, '/');
			driver_name = driver_name ? driver_name + 1 : link_target;
			snprintf(gpu->driver, sizeof(gpu->driver), "%s", driver_name);
		}

		/* Get PCI slot from uevent */
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/uevent", card_num);
		FILE *f = fopen(path, "r");
		if (f) {
			char line[256];
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
					char *nl = strchr(line + 14, '\n');
					if (nl) *nl = '\0';
					snprintf(gpu->pci_slot, sizeof(gpu->pci_slot), "%s", line + 14);
					/* Create underscore version for DRI_PRIME */
					snprintf(gpu->pci_slot_underscore, sizeof(gpu->pci_slot_underscore),
						"pci-%s", gpu->pci_slot);
					for (char *p = gpu->pci_slot_underscore; *p; p++) {
						if (*p == ':' || *p == '.') *p = '_';
					}
					break;
				}
			}
			fclose(f);
		}

		/* Determine vendor and if discrete */
		if (strcmp(gpu->driver, "nvidia") == 0) {
			gpu->vendor = GPU_VENDOR_NVIDIA;
			gpu->is_discrete = 1;
		} else if (strcmp(gpu->driver, "nouveau") == 0) {
			gpu->vendor = GPU_VENDOR_NVIDIA;
			gpu->is_discrete = 1;
		} else if (strcmp(gpu->driver, "amdgpu") == 0 || strcmp(gpu->driver, "radeon") == 0) {
			gpu->vendor = GPU_VENDOR_AMD;
			/* Check if integrated (APU) or discrete - look at device class or boot_vga */
			snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/boot_vga", card_num);
			f = fopen(path, "r");
			if (f) {
				int boot_vga = 0;
				if (fscanf(f, "%d", &boot_vga) == 1) {
					/* boot_vga=1 typically means primary/integrated on laptops */
					/* But for AMD, we also check if another GPU exists */
					gpu->is_discrete = !boot_vga;
				}
				fclose(f);
			} else {
				/* If no boot_vga, assume discrete */
				gpu->is_discrete = 1;
			}
		} else if (strcmp(gpu->driver, "i915") == 0 || strcmp(gpu->driver, "xe") == 0) {
			gpu->vendor = GPU_VENDOR_INTEL;
			gpu->is_discrete = 0; /* Intel iGPU */
		} else {
			gpu->vendor = GPU_VENDOR_UNKNOWN;
			gpu->is_discrete = 0;
		}

		wlr_log(WLR_INFO, "GPU %d: %s [%s] driver=%s discrete=%d pci=%s",
			detected_gpu_count, gpu->card_path, gpu->render_path,
			gpu->driver, gpu->is_discrete, gpu->pci_slot);

		/* Validate render node exists and is accessible */
		if (gpu->render_path[0]) {
			if (access(gpu->render_path, F_OK) != 0) {
				wlr_log(WLR_ERROR,
					"GPU %d: render node %s does NOT exist — "
					"check that kernel module '%s' is loaded correctly",
					detected_gpu_count, gpu->render_path, gpu->driver);
			} else if (access(gpu->render_path, R_OK | W_OK) != 0) {
				wlr_log(WLR_ERROR,
					"GPU %d: render node %s is not accessible (permission denied) — "
					"add your user to the 'render' and/or 'video' group: "
					"sudo usermod -aG render,video $USER",
					detected_gpu_count, gpu->render_path);
			}
		} else {
			wlr_log(WLR_ERROR,
				"GPU %d: no render node found for %s — "
				"check that kernel module '%s' is loaded and "
				"/dev/dri/renderD* devices exist",
				detected_gpu_count, gpu->card_path, gpu->driver);
		}

		/* Validate card node accessibility */
		if (access(gpu->card_path, F_OK) == 0 &&
		    access(gpu->card_path, R_OK | W_OK) != 0) {
			wlr_log(WLR_ERROR,
				"GPU %d: card node %s is not accessible (permission denied) — "
				"check that a session manager (logind/seatd) is running and "
				"your user is in the 'video' group",
				detected_gpu_count, gpu->card_path);
		}

		detected_gpu_count++;
	}
	closedir(dri_dir);

	/* Find best discrete and integrated GPU */
	for (i = 0; i < detected_gpu_count; i++) {
		GpuInfo *gpu = &detected_gpus[i];
		if (gpu->is_discrete) {
			/* Prefer Nvidia > AMD for discrete */
			if (discrete_gpu_idx < 0 ||
			    (gpu->vendor == GPU_VENDOR_NVIDIA && detected_gpus[discrete_gpu_idx].vendor != GPU_VENDOR_NVIDIA)) {
				discrete_gpu_idx = i;
			}
		} else {
			if (integrated_gpu_idx < 0)
				integrated_gpu_idx = i;
		}
	}

	/* If AMD GPU was marked as not-discrete but no other integrated exists, it might be wrong */
	if (discrete_gpu_idx < 0 && detected_gpu_count > 1) {
		/* Multiple GPUs but none marked discrete - pick non-Intel as discrete */
		for (i = 0; i < detected_gpu_count; i++) {
			if (detected_gpus[i].vendor != GPU_VENDOR_INTEL) {
				discrete_gpu_idx = i;
				detected_gpus[i].is_discrete = 1;
				break;
			}
		}
	}

	if (discrete_gpu_idx >= 0) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];
		wlr_log(WLR_INFO, "Selected discrete GPU: card%d (%s) driver=%s",
			dgpu->card_index, dgpu->pci_slot, dgpu->driver);
	} else {
		wlr_log(WLR_INFO, "No discrete GPU detected");
	}

	/*
	 * Prevent dGPU D3cold/runtime-suspend permanently.
	 *
	 * Three layers of protection against the GPU switching to integrated:
	 *
	 * 1. Set power/control=on + autosuspend_delay_ms=-1 on the GPU and ALL
	 *    sibling PCI functions (audio, USB-C, etc).  This disables the
	 *    kernel's runtime PM for the PCI device.
	 *
	 * 2. Hold an open fd on the dGPU's render node (/dev/dri/renderDXXX)
	 *    for the compositor's entire lifetime.  This keeps the GPU "in use"
	 *    from the driver's perspective, preventing NVIDIA's internal dynamic
	 *    power management from powering off the GPU even if something
	 *    external (nvidia-powerd, udev, power-profiles-daemon, tlp)
	 *    re-enables runtime PM in sysfs.
	 *
	 * 3. A periodic watchdog timer (dgpu_power_watchdog_tick) re-asserts
	 *    the sysfs power settings every 10 seconds, counteracting any
	 *    external service that overrides them.
	 */
	if (discrete_gpu_idx >= 0) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];

		dgpu_assert_power_on(dgpu);

		/*
		 * Hold the render node open — prevents the driver from
		 * considering the GPU idle and triggering D3cold.
		 */
		if (dgpu->render_path[0]) {
			dgpu_render_fd = open(dgpu->render_path, O_RDWR);
			if (dgpu_render_fd >= 0) {
				wlr_log(WLR_INFO, "dGPU: holding %s open (fd=%d) to prevent D3cold",
					dgpu->render_path, dgpu_render_fd);
			} else {
				wlr_log(WLR_ERROR, "dGPU: failed to open %s: %s",
					dgpu->render_path, strerror(errno));
			}
		}

		/*
		 * Check NVreg_DynamicPowerManagement — if set to 0x02 (fine-grained),
		 * the NVIDIA driver has its own idle timeout that can override sysfs.
		 * This is a boot-time parameter and cannot be changed at runtime.
		 */
		if (dgpu->vendor == GPU_VENDOR_NVIDIA) {
			/* CPU cursor buffer handles HW cursor plane via dumb DRM
			 * buffers, bypassing broken GBM allocation.  No need to
			 * force software cursors anymore. */

			/* GBM_BACKEND=nvidia-drm: only set when the compositor itself
			 * renders on Nvidia (single-GPU mode).  In hybrid mode the
			 * compositor renders on the Intel iGPU — setting GBM_BACKEND
			 * here would be wrong (Intel FD + Nvidia GBM backend) AND
			 * the var is inherited by ALL child processes, causing
			 * non-dgpu clients to mis-allocate buffers on Nvidia →
			 * cross-GPU DMA-BUF import failures (invisible windows).
			 * For child processes that need the dGPU, set_dgpu_env()
			 * already sets GBM_BACKEND=nvidia-drm per-process. */
			if (integrated_gpu_idx < 0)
				setenv("GBM_BACKEND", "nvidia-drm", 0);

			/* Detect Nvidia driver version for explicit sync support.
			 * Driver 555+ is required for DRM syncobj (explicit sync)
			 * which eliminates Xwayland flickering.  Older drivers
			 * also need legacy DRM modesetting as a fallback. */
			{
				char nv_ver[32] = "";
				int ver_fd = open("/sys/module/nvidia/version", O_RDONLY);
				if (ver_fd >= 0) {
					ssize_t n = read(ver_fd, nv_ver, sizeof(nv_ver) - 1);
					close(ver_fd);
					if (n > 0) {
						nv_ver[n] = '\0';
						while (n > 0 && (nv_ver[n-1] == '\n' || nv_ver[n-1] == ' '))
							nv_ver[--n] = '\0';
						int major = atoi(nv_ver);
						dgpu->driver_version = major;
						wlr_log(WLR_INFO, "NVIDIA: driver version %s (major=%d)", nv_ver, major);
						if (major > 0 && major < 555) {
							wlr_log(WLR_ERROR,
								"NVIDIA: driver %s is too old for explicit sync. "
								"Xwayland WILL flicker. Upgrade to 555+ for proper "
								"Wayland/Xwayland support.", nv_ver);
							/* Older Nvidia drivers have atomic modesetting bugs */
							setenv("WLR_DRM_NO_ATOMIC", "1", 0);
							/* Old drivers have incomplete modifier support —
							 * many modifiers reported as "supported" cause
							 * buffer allocation/import failures.  Driver 555+
							 * handles modifiers correctly and NEEDS them:
							 * without modifier negotiation GBM allocates tiled
							 * buffers but reports modifier=INVALID, causing
							 * KMS import to fail on all paths. */
							setenv("WLR_DRM_NO_MODIFIERS", "1", 0);
						}
					}
				} else if (strcmp(dgpu->driver, "nvidia") == 0) {
					wlr_log(WLR_ERROR,
						"NVIDIA: could not read driver version from "
						"/sys/module/nvidia/version — ensure nvidia-drm.modeset=1 "
						"kernel parameter is set");
				}
			}

			/* nvidia-drm.fbdev check — improves scanout performance.
			 * Auto-enabled in driver 570+, needs manual set for 555-569. */
			if (dgpu->driver_version >= 555 && dgpu->driver_version < 570) {
				if (!check_module_param("nvidia_drm", "fbdev", "Y") &&
				    !check_module_param("nvidia_drm", "fbdev", "1")) {
					struct utsname uts;
					if (uname(&uts) == 0) {
						int kmaj = 0, kmin = 0;
						sscanf(uts.release, "%d.%d", &kmaj, &kmin);
						if (kmaj > 6 || (kmaj == 6 && kmin >= 11)) {
							wlr_log(WLR_ERROR,
								"NVIDIA: nvidia-drm.fbdev=1 recommended on kernel %d.%d "
								"(driver %d). Add 'nvidia-drm.fbdev=1' to kernel parameters "
								"for better scanout performance.",
								kmaj, kmin, dgpu->driver_version);
						}
					}
				}
			}

			/* GSP firmware status — informational for diagnostics */
			{
				char gsp_val[16] = "";
				int gsp_fd = open("/sys/module/nvidia/parameters/NVreg_EnableGpuFirmware", O_RDONLY);
				if (gsp_fd >= 0) {
					ssize_t gn = read(gsp_fd, gsp_val, sizeof(gsp_val) - 1);
					close(gsp_fd);
					if (gn > 0) {
						gsp_val[gn] = '\0';
						while (gn > 0 && (gsp_val[gn-1] == '\n' || gsp_val[gn-1] == ' '))
							gsp_val[--gn] = '\0';
						if (strcmp(gsp_val, "1") == 0 || strcmp(gsp_val, "Y") == 0) {
							wlr_log(WLR_INFO, "NVIDIA: GSP firmware enabled (good)");
						} else if (dgpu->driver_version >= 560) {
							wlr_log(WLR_INFO,
								"NVIDIA: GSP firmware disabled (NVreg_EnableGpuFirmware=%s). "
								"GSP is recommended for driver %d+ for improved performance "
								"and power management.", gsp_val, dgpu->driver_version);
						} else {
							wlr_log(WLR_INFO, "NVIDIA: GSP firmware status: %s", gsp_val);
						}
					}
				}
			}

			/* NVreg_PreserveVideoMemoryAllocations — suspend/resume support */
			{
				char pvm_val[16] = "";
				int pvm_fd = open("/sys/module/nvidia/parameters/NVreg_PreserveVideoMemoryAllocations", O_RDONLY);
				if (pvm_fd >= 0) {
					ssize_t pn = read(pvm_fd, pvm_val, sizeof(pvm_val) - 1);
					close(pvm_fd);
					if (pn > 0) {
						pvm_val[pn] = '\0';
						while (pn > 0 && (pvm_val[pn-1] == '\n' || pvm_val[pn-1] == ' '))
							pvm_val[--pn] = '\0';
						if (strcmp(pvm_val, "1") == 0 || strcmp(pvm_val, "Y") == 0) {
							wlr_log(WLR_INFO,
								"NVIDIA: NVreg_PreserveVideoMemoryAllocations=1 (suspend/resume safe)");
						} else {
							wlr_log(WLR_INFO,
								"NVIDIA: NVreg_PreserveVideoMemoryAllocations=%s — "
								"set to 1 and enable nvidia-suspend/resume systemd services "
								"for proper suspend/resume support.", pvm_val);
						}
					}
				}
			}

			char dpm_val[16] = "";
			int dpm_fd = open("/sys/module/nvidia/parameters/NVreg_DynamicPowerManagement", O_RDONLY);
			if (dpm_fd >= 0) {
				ssize_t n = read(dpm_fd, dpm_val, sizeof(dpm_val) - 1);
				close(dpm_fd);
				if (n > 0) {
					dpm_val[n] = '\0';
					while (n > 0 && (dpm_val[n-1] == '\n' || dpm_val[n-1] == ' '))
						dpm_val[--n] = '\0';
					if (strstr(dpm_val, "2") || strstr(dpm_val, "0x02")) {
						wlr_log(WLR_ERROR,
							"NVIDIA: WARNING — NVreg_DynamicPowerManagement=%s (fine-grained). "
							"This allows the driver to independently power off the dGPU. "
							"Set nvidia.NVreg_DynamicPowerManagement=0x00 in boot params to disable.",
							dpm_val);
					} else {
						wlr_log(WLR_INFO, "NVIDIA: NVreg_DynamicPowerManagement=%s", dpm_val);
					}
				}
			}
		}

		wlr_log(WLR_INFO, "dGPU: D3cold prevention active — power/control=on + render node held open");
	}

	/* Vendor-specific kernel module validation */
	for (i = 0; i < detected_gpu_count; i++) {
		GpuInfo *gpu = &detected_gpus[i];
		switch (gpu->vendor) {
		case GPU_VENDOR_NVIDIA:
			if (!validate_kernel_module("nvidia_drm")) {
				wlr_log(WLR_ERROR,
					"GPU %d (NVIDIA): nvidia_drm kernel module is NOT loaded — "
					"DRM/KMS will not work. Load it with: "
					"modprobe nvidia_drm modeset=1",
					gpu->card_index);
			} else if (!check_module_param("nvidia_drm", "modeset", "Y") &&
			           !check_module_param("nvidia_drm", "modeset", "1")) {
				wlr_log(WLR_ERROR,
					"GPU %d (NVIDIA): nvidia_drm.modeset is NOT enabled — "
					"Wayland requires modesetting. Add "
					"'nvidia-drm.modeset=1' to kernel parameters",
					gpu->card_index);
			}
			break;
		case GPU_VENDOR_INTEL:
			if (!validate_kernel_module("i915") && !validate_kernel_module("xe")) {
				wlr_log(WLR_ERROR,
					"GPU %d (Intel): neither i915 nor xe kernel module is loaded — "
					"Intel GPU will not function. Check kernel config or "
					"load module: modprobe i915",
					gpu->card_index);
			}
			break;
		case GPU_VENDOR_AMD:
			if (!validate_kernel_module("amdgpu")) {
				wlr_log(WLR_ERROR,
					"GPU %d (AMD): amdgpu kernel module is NOT loaded — "
					"AMD GPU will not function. Load it with: "
					"modprobe amdgpu",
					gpu->card_index);
			}
			break;
		default:
			break;
		}
	}
}

/*
 * Assert power/control=on on the dGPU and all its PCI siblings.
 * Called at startup and periodically by the watchdog timer.
 */
void
dgpu_assert_power_on(GpuInfo *gpu)
{
	char pci_path[128];
	int fd;

	if (!gpu || !gpu->pci_slot[0])
		return;

	/* Main GPU function */
	snprintf(pci_path, sizeof(pci_path),
		"/sys/bus/pci/devices/%s/power/control", gpu->pci_slot);
	fd = open(pci_path, O_WRONLY);
	if (fd >= 0) {
		write(fd, "on", 2);
		close(fd);
	}
	snprintf(pci_path, sizeof(pci_path),
		"/sys/bus/pci/devices/%s/power/autosuspend_delay_ms", gpu->pci_slot);
	fd = open(pci_path, O_WRONLY);
	if (fd >= 0) {
		write(fd, "-1", 2);
		close(fd);
	}

	/* All sibling PCI functions (.1 audio, .2 USB-C, etc) */
	char slot_prefix[64];
	strncpy(slot_prefix, gpu->pci_slot, sizeof(slot_prefix) - 1);
	slot_prefix[sizeof(slot_prefix) - 1] = '\0';
	char *dot = strrchr(slot_prefix, '.');
	if (dot) {
		*dot = '\0';
		for (int fn = 1; fn <= 7; fn++) {
			snprintf(pci_path, sizeof(pci_path),
				"/sys/bus/pci/devices/%s.%d/power/control", slot_prefix, fn);
			fd = open(pci_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "on", 2);
				close(fd);
			}
			snprintf(pci_path, sizeof(pci_path),
				"/sys/bus/pci/devices/%s.%d/power/autosuspend_delay_ms", slot_prefix, fn);
			fd = open(pci_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "-1", 2);
				close(fd);
			}
		}
	}
}

/*
 * Watchdog timer callback — re-assert dGPU power/control=on every 60s.
 * Counteracts nvidia-powerd, power-profiles-daemon, tlp, udev rules,
 * or any other service that periodically re-enables runtime PM.
 */
static int
dgpu_power_watchdog_tick(void *data)
{
	(void)data;

	if (discrete_gpu_idx >= 0 && discrete_gpu_idx < detected_gpu_count) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];
		dgpu_assert_power_on(dgpu);

		/* Verify the render node fd is still open */
		if (dgpu_render_fd >= 0 && fcntl(dgpu_render_fd, F_GETFD) < 0) {
			wlr_log(WLR_ERROR, "dGPU: render node fd lost, re-opening %s",
				dgpu->render_path);
			dgpu_render_fd = open(dgpu->render_path, O_RDWR);
		}
	}

	/* Re-arm timer */
	if (dgpu_power_watchdog)
		wl_event_source_timer_update(dgpu_power_watchdog, 60000);
	return 0;
}

void
dgpu_power_watchdog_start(void)
{
	if (discrete_gpu_idx < 0 || dgpu_power_watchdog)
		return;
	dgpu_power_watchdog = wl_event_loop_add_timer(event_loop,
		dgpu_power_watchdog_tick, NULL);
	if (dgpu_power_watchdog)
		wl_event_source_timer_update(dgpu_power_watchdog, 60000);
}

int
should_use_dgpu(const char *cmd)
{
	const char *base;
	int i;

	if (!cmd)
		return 0;

	/* Get basename of command */
	base = strrchr(cmd, '/');
	base = base ? base + 1 : cmd;

	for (i = 0; dgpu_programs[i]; i++) {
		if (strcasecmp(base, dgpu_programs[i]) == 0)
			return 1;
		/* Also check if command contains the program name (for wrappers) */
		if (strcasestr(cmd, dgpu_programs[i]))
			return 1;
	}
	return 0;
}

void
set_dgpu_env(void)
{
	GpuInfo *dgpu;

	/* No discrete GPU detected - nothing to do */
	if (discrete_gpu_idx < 0 || discrete_gpu_idx >= detected_gpu_count)
		return;

	dgpu = &detected_gpus[discrete_gpu_idx];

	switch (dgpu->vendor) {
	case GPU_VENDOR_NVIDIA:
		/* GLX vendor — always needed so GLX uses NVIDIA */
		setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
		setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);

		/* GBM backend — helps libgbm find NVIDIA's implementation */
		setenv("GBM_BACKEND", "nvidia-drm", 0);

		/* PRIME render offload — ONLY for hybrid (iGPU + dGPU) systems.
		 * On NVIDIA-only, these cause Xwayland EGL init to fail. */
		if (integrated_gpu_idx >= 0) {
			setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
			setenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER", "NVIDIA-G0", 1);
			if (dgpu->pci_slot_underscore[0])
				setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
			else
				setenv("DRI_PRIME", "1", 1);

			/* Force Qt/GTK apps through Xwayland when PRIME offload
			 * is active.  Qt6's wayland-egl plugin cannot create a
			 * valid EGLConfig with the NVIDIA PRIME env vars, causing
			 * a SIGSEGV in QEGLPlatformContext (FreeCAD, KiCad, etc.).
			 * Games don't use QT_QPA_PLATFORM, so this is harmless
			 * for them — they go via Xwayland/gamescope anyway. */
			setenv("QT_QPA_PLATFORM", "xcb", 1);
			setenv("GDK_BACKEND", "x11", 1);

			wlr_log(WLR_INFO, "NVIDIA: hybrid mode (iGPU + dGPU), PRIME offload enabled");
		} else {
			wlr_log(WLR_INFO, "NVIDIA: single-GPU mode, PRIME offload skipped");
		}

		/* DRI_PRIME for nouveau (doesn't use PRIME offload vars) */
		if (strcmp(dgpu->driver, "nouveau") == 0 && integrated_gpu_idx >= 0) {
			if (dgpu->pci_slot_underscore[0])
				setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
			else
				setenv("DRI_PRIME", "1", 1);
		}

		/*
		 * NVIDIA frame pacing & performance environment variables.
		 * These are critical for optimal game performance under Wayland.
		 */

		/* Limit pre-rendered frames to 1 — minimizes input latency.
		 * The compositor handles vsync/frame pacing, so deep queuing
		 * only adds lag without benefit. */
		setenv("__GL_MaxFramesAllowed", "1", 0);

		/* Disable driver-side vsync — the Wayland compositor handles
		 * presentation timing. Double-vsync causes input lag. */
		setenv("__GL_SYNC_TO_VBLANK", "0", 0);

		/* Allow G-Sync/VRR — the compositor enables adaptive sync
		 * for fullscreen games automatically. */
		setenv("__GL_GSYNC_ALLOWED", "1", 0);
		setenv("__GL_VRR_ALLOWED", "1", 0);

		/* Don't yield CPU while waiting for GPU — reduces latency
		 * at the cost of slightly higher CPU usage. */
		setenv("__GL_YIELD", "NOTHING", 0);

		/* Enable threaded OpenGL optimizations — offloads GL work
		 * to a separate thread for better CPU utilization. */
		setenv("__GL_THREADED_OPTIMIZATIONS", "1", 0);

		/* Persistent shader cache — avoid re-compilation stutters.
		 * Don't clean up old shaders to keep cache warm. */
		setenv("__GL_SHADER_DISK_CACHE", "1", 0);
		setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 0);

		/* Use direct rendering for lowest latency (skip composition queue) */
		setenv("__GL_ALLOW_UNOFFICIAL_PROTOCOL", "1", 0);

		/* Xwayland / EGL session type — ensures X11 and Wayland clients
		 * correctly identify the session as Wayland. */
		setenv("XDG_SESSION_TYPE", "wayland", 0);

		/* VA-API hardware video decode via Nvidia */
		setenv("LIBVA_DRIVER_NAME", "nvidia", 0);
		setenv("NVD_BACKEND", "direct", 0);
		setenv("VDPAU_DRIVER", "nvidia", 0);

		/* Ensure direct rendering (not indirect/software) */
		setenv("LIBGL_ALWAYS_INDIRECT", "0", 0);

		/* Nvidia experimental performance strategy */
		setenv("__GL_ExperimentalPerfStrategy", "1", 0);

		/* Allow FXAA if app requests it */
		setenv("__GL_ALLOW_FXAA_USAGE", "1", 0);

		/* Nvidia image sharpening — let apps control this */
		setenv("__GL_SHARPEN_ENABLE", "0", 0);
		setenv("__GL_SHARPEN_VALUE", "0", 0);
		setenv("__GL_SHARPEN_IGNORE_FILM_GRAIN", "0", 0);

		/* Version-based diagnostics */
		if (dgpu->driver_version > 0 && dgpu->driver_version < 570) {
			wlr_log(WLR_INFO,
				"NVIDIA: driver %d < 570 — consider adding 'nvidia-drm.fbdev=1' "
				"to kernel parameters for better scanout performance.",
				dgpu->driver_version);
		}
		if (dgpu->driver_version >= 565) {
			wlr_log(WLR_INFO,
				"NVIDIA: driver %d — GLX_EXT_buffer_age re-enabled (good for compositors)",
				dgpu->driver_version);
		}

		wlr_log(WLR_INFO,
			"NVIDIA: compositor env set — GBM_BACKEND=nvidia-drm, "
			"__GLX_VENDOR_LIBRARY_NAME=nvidia (CPU cursor buffer for HW cursor)"
			"%s",
			dgpu->driver_version > 0 && dgpu->driver_version < 555
				? ", WLR_DRM_NO_MODIFIERS=1 (old driver)" : "");

		break;

	case GPU_VENDOR_AMD:
		/* DRI_PRIME with specific PCI device for AMD */
		if (dgpu->pci_slot_underscore[0])
			setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
		else
			setenv("DRI_PRIME", "1", 1);
		/* AMD Vulkan driver selection */
		setenv("AMD_VULKAN_ICD", "RADV", 1);
		break;

	case GPU_VENDOR_INTEL:
	case GPU_VENDOR_UNKNOWN:
	default:
		/* Generic DRI_PRIME for other cases */
		if (dgpu->pci_slot_underscore[0])
			setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
		else
			setenv("DRI_PRIME", "1", 1);
		break;
	}

	/* Vulkan device selection - prefer discrete GPU.
	 *
	 * Multiple layers of selection to guarantee dGPU usage:
	 *
	 * 1) VK_LOADER_DRIVERS_SELECT — modern Vulkan loader (1.3.234+)
	 *    filters ICD files by filename glob.
	 *
	 * 2) MESA_VK_DEVICE_SELECT — Mesa's VkLayer_MESA_device_select
	 *    implicit layer, selects device by PCI vendor:device ID.
	 *    Without this, the layer defaults to boot_vga (= iGPU).
	 *
	 * 3) DXVK_FILTER_DEVICE_NAME — DXVK/Proton device selection by
	 *    name substring.  Guarantees Proton games use the dGPU.
	 */
	switch (dgpu->vendor) {
	case GPU_VENDOR_NVIDIA:
		setenv("VK_LOADER_DRIVERS_SELECT", "nvidia*", 1);
		/* PCI vendor 0x10de = NVIDIA */
		setenv("MESA_VK_DEVICE_SELECT", "10de:", 1);
		setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
		setenv("DXVK_FILTER_DEVICE_NAME", "NVIDIA", 1);
		/* Ensure Proton exposes NVIDIA GPU to games */
		setenv("PROTON_HIDE_NVIDIA_GPU", "0", 1);
		setenv("PROTON_ENABLE_NVAPI", "1", 1);
		setenv("DXVK_ENABLE_NVAPI", "1", 1);
		break;
	case GPU_VENDOR_AMD:
		setenv("VK_LOADER_DRIVERS_SELECT", "radeon*,amd*", 1);
		/* PCI vendor 0x1002 = AMD */
		setenv("MESA_VK_DEVICE_SELECT", "1002:", 1);
		setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
		setenv("DXVK_FILTER_DEVICE_NAME", "AMD", 1);
		break;
	default:
		setenv("VK_LOADER_DRIVERS_SELECT", "nvidia*,radeon*,amd*", 1);
		break;
	}
}

void
set_steam_env(void)
{
	GpuInfo *dgpu;

	/*
	 * AUDIO: Ensure Steam and games use PipeWire for audio
	 *
	 * SDL_AUDIODRIVER=pipewire: Tell SDL (used by many games) to use PipeWire directly
	 * PULSE_SERVER: Point PulseAudio clients to PipeWire's pulse socket
	 * PIPEWIRE_RUNTIME_DIR: Ensure PipeWire socket is found
	 *
	 * Note: Modern PipeWire provides PulseAudio compatibility, so most games
	 * will work via the pulse socket even without explicit PipeWire support.
	 */
	setenv("SDL_AUDIODRIVER", "pipewire,pulseaudio,alsa", 0); /* Don't override if already set */

	/* Ensure XDG_RUNTIME_DIR is set (needed for PipeWire socket) */
	if (!getenv("XDG_RUNTIME_DIR")) {
		char runtime_dir[256];
		snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", getuid());
		setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	}

	/* For Proton/Wine games: use PipeWire via the PulseAudio interface */
	setenv("PULSE_LATENCY_MSEC", "60", 0); /* Reasonable latency for games */

	/* UI scaling fix for better Big Picture performance on hybrid systems */
	setenv("STEAM_FORCE_DESKTOPUI_SCALING", "1", 1);

	/* Tell Steam to prefer host libraries over runtime for better driver compat */
	setenv("STEAM_RUNTIME_PREFER_HOST_LIBRARIES", "1", 1);

	/* CEF/Chromium GPU override flags - force hardware acceleration
	 * These help bypass Steam's internal GPU blocklist on hybrid systems */
	setenv("STEAM_DISABLE_GPU_BLOCKLIST", "1", 1);
	setenv("STEAM_CEF_ARGS", "--ignore-gpu-blocklist --enable-gpu-rasterization --enable-zero-copy --enable-native-gpu-memory-buffers --disable-gpu-driver-bug-workarounds", 1);

	/* Override CEF webhelper flags - Steam internally adds --disable-gpu which kills performance
	 * STEAM_WEBHELPER_CEF_FLAGS can override/append to steamwebhelper arguments */
	setenv("STEAM_WEBHELPER_CEF_FLAGS", "--enable-gpu --enable-gpu-compositing --enable-accelerated-video-decode --ignore-gpu-blocklist", 1);

	/* Set dGPU env for Steam — this calls set_dgpu_env() which handles
	 * ALL GPU vendor-specific vars including Vulkan device selection.
	 * Redundant with the compositor-level call, but ensures the vars
	 * are present even if Steam was started before GPU detection. */
	set_dgpu_env();

	if (discrete_gpu_idx >= 0 && discrete_gpu_idx < detected_gpu_count) {
		dgpu = &detected_gpus[discrete_gpu_idx];
		if (dgpu->vendor == GPU_VENDOR_NVIDIA) {
			/* NVIDIA-specific CEF args for better ANGLE/Vulkan support */
			setenv("STEAM_CEF_GPU_ARGS", "--use-angle=vulkan --enable-features=Vulkan,UseSkiaRenderer", 1);
		}
	}
}


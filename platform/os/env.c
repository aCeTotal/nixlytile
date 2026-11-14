#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/os/env.h"

bool env_parse_bool(const char *option) {
	const char *env = getenv(option);
	if (env == NULL) {
		return false;
	}
	nx_log(NX_INFO, "Loading %s option: %s", option, env);

	if (strcmp(env, "0") == 0) {
		return false;
	}
	if (strcmp(env, "1") == 0) {
		return true;
	}
	nx_log(NX_ERROR, "Unknown %s option: %s", option, env);
	return false;
}

size_t env_parse_switch(const char *option, const char **switches) {
	const char *env = getenv(option);
	if (env == NULL) {
		return 0;
	}
	nx_log(NX_INFO, "Loading %s option: %s", option, env);

	for (size_t i = 0; switches[i]; i++) {
		if (strcmp(env, switches[i]) == 0) {
			return i;
		}
	}
	nx_log(NX_ERROR, "Unknown %s option: %s", option, env);
	return 0;
}

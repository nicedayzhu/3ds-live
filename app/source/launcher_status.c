#include "launcher_status.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_number(const char *text, unsigned base, uint32_t *value)
{
	const char *digits =
	    base == 10 ? "0123456789" : "0123456789abcdefABCDEF";
	const size_t length = strlen(text);
	if (!length || (base == 16 && length != 8) ||
	    strspn(text, digits) != length)
		return false;
	char *end;
	errno = 0;
	const unsigned long number = strtoul(text, &end, (int)base);
	if (errno == ERANGE || *end || number > UINT32_MAX)
		return false;
	*value = (uint32_t)number;
	return true;
}

static bool display_state(const char *state, const char *led,
			  LauncherState *out)
{
	if (strcmp(state, "idle") == 0) {
		if (strcmp(led, "ready") == 0)
			*out = LAUNCHER_READY;
		else if (strcmp(led, "off") == 0)
			*out = LAUNCHER_STOPPED;
		else
			return false;
	} else if (strcmp(state, "streaming") == 0) {
		if (strcmp(led, "receiving") == 0)
			*out = LAUNCHER_RECEIVING;
		else if (strcmp(led, "waiting") == 0)
			*out = LAUNCHER_PAIRED;
		else
			return false;
	} else if (strcmp(state, "searching") == 0 &&
		   strcmp(led, "waiting") == 0) {
		*out = LAUNCHER_SEARCHING;
	} else if (strcmp(state, "sleep") == 0 && strcmp(led, "off") == 0) {
		*out = LAUNCHER_SLEEP;
	} else if (strcmp(state, "error") == 0 && strcmp(led, "error") == 0) {
		*out = LAUNCHER_ERROR;
	} else if (strcmp(state, "stopped") == 0 && strcmp(led, "off") == 0) {
		*out = LAUNCHER_STOPPED;
	} else
		return false;
	return true;
}

bool launcher_read_status(const char *path, uint32_t active_pid,
			  const char *expected_build, LauncherStatus *out)
{
	if (!active_pid || !path || !expected_build || !*expected_build || !out)
		return false;
	FILE *file = fopen(path, "r");
	if (!file)
		return false;
	LauncherStatus next = {0};
	enum {
		PID = 1,
		BUILD = 2,
		STATE = 4,
		RESULT = 8,
		LED = 16,
		LED_RESULT = 32
	};
	unsigned seen = 0;
	char line[96], state[16] = "", led[16] = "";
	bool valid = true;
	while (valid && fgets(line, sizeof line, file)) {
		if (!strchr(line, '\n')) {
			valid = false;
			break;
		}
		line[strcspn(line, "\r\n")] = '\0';
		char *value = strchr(line, '=');
		if (!value)
			continue;
		*value++ = '\0';
		unsigned field;
		if (strcmp(line, "pid") == 0) {
			uint32_t pid;
			field = PID;
			valid =
			    parse_number(value, 10, &pid) && pid == active_pid;
		} else if (strcmp(line, "build") == 0) {
			field = BUILD;
			valid = strcmp(value, expected_build) == 0;
		} else if (strcmp(line, "state") == 0) {
			field = STATE;
			valid = strlen(value) < sizeof state;
			if (valid)
				strcpy(state, value);
		} else if (strcmp(line, "result") == 0) {
			field = RESULT;
			valid = parse_number(value, 16, &next.result);
		} else if (strcmp(line, "led") == 0) {
			field = LED;
			valid = strlen(value) < sizeof led;
			if (valid)
				strcpy(led, value);
		} else if (strcmp(line, "led_result") == 0) {
			field = LED_RESULT;
			valid = parse_number(value, 16, &next.led_result);
		} else
			continue;
		if (seen & field)
			valid = false;
		seen |= field;
	}
	valid = valid && !ferror(file) &&
		seen == (PID | BUILD | STATE | RESULT | LED | LED_RESULT) &&
		display_state(state, led, &next.state);
	fclose(file);
	if (valid)
		*out = next;
	return valid;
}

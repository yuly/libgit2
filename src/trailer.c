/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "common.h"
#include "git2/commit.h"
#include "git2/trailer.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>

static const char comment_line_char = '#';

static const char *const git_generated_prefixes[] = {
	"Signed-off-by: ",
	"(cherry picked from commit ",
	NULL
};

static const char *const separators = ":";

static inline int is_blank_line(const char *str)
{
	const char *s = str;
	while (*s && *s != '\n' && isspace(*s))
		s++;
	return !*s || *s == '\n';
}

static const char *next_line(const char *str)
{
	const char *nl = strchr(str, '\n');

	if (nl) {
		return nl + 1;
	} else {
		return NULL;
	}
}

/*
 * Return the position of the start of the last line. If len is 0, return -1.
 */
static int last_line(const char *buf, size_t len)
{
	int i;
	if (len == 0)
		return -1;
	if (len == 1)
		return 0;
	/*
	 * Skip the last character (in addition to the null terminator),
	 * because if the last character is a newline, it is considered as part
	 * of the last line anyway.
	 */
	i = len - 2;

	for (; i >= 0; i--) {
		if (buf[i] == '\n')
			return i + 1;
	}
	return 0;
}

static bool starts_with(const char *str, const char *prefix)
{
	size_t str_len = strlen(str);
	size_t prefix_len = strlen(prefix);

	if (prefix_len > str_len) {
		return false;
	}

	return memcmp(str, prefix, prefix_len) == 0;
}

/*
 * If the given line is of the form
 * "<token><optional whitespace><separator>..." or "<separator>...", return the
 * location of the separator. Otherwise, return -1.  The optional whitespace
 * is allowed there primarily to allow things like "Bug #43" where <token> is
 * "Bug" and <separator> is "#".
 *
 * The separator-starts-line case (in which this function returns 0) is
 * distinguished from the non-well-formed-line case (in which this function
 * returns -1) because some callers of this function need such a distinction.
 */
static int find_separator(const char *line, const char *separators)
{
	int whitespace_found = 0;
	const char *c;
	for (c = line; *c; c++) {
		if (strchr(separators, *c))
			return c - line;
		if (!whitespace_found && (isalnum(*c) || *c == '-'))
			continue;
		if (c != line && (*c == ' ' || *c == '\t')) {
			whitespace_found = 1;
			continue;
		}
		break;
	}
	return -1;
}

/*
 * Inspect the given string and determine the true "end" of the log message, in
 * order to find where to put a new Signed-off-by: line.  Ignored are
 * trailing comment lines and blank lines.  To support "git commit -s
 * --amend" on an existing commit, we also ignore "Conflicts:".  To
 * support "git commit -v", we truncate at cut lines.
 *
 * Returns the number of bytes from the tail to ignore, to be fed as
 * the second parameter to append_signoff().
 */
int ignore_non_trailer(const char *buf, size_t len)
{
	int boc = 0;
	size_t bol = 0;
	int in_old_conflicts_block = 0;
	size_t cutoff = len;

	while (bol < cutoff) {
		const char *next_line = memchr(buf + bol, '\n', len - bol);

		if (!next_line)
			next_line = buf + len;
		else
			next_line++;

		if (buf[bol] == comment_line_char || buf[bol] == '\n') {
			/* is this the first of the run of comments? */
			if (!boc)
				boc = bol;
			/* otherwise, it is just continuing */
		} else if (starts_with(buf + bol, "Conflicts:\n")) {
			in_old_conflicts_block = 1;
			if (!boc)
				boc = bol;
		} else if (in_old_conflicts_block && buf[bol] == '\t') {
			; /* a pathname in the conflicts block */
		} else if (boc) {
			/* the previous was not trailing comment */
			boc = 0;
			in_old_conflicts_block = 0;
		}
		bol = next_line - buf;
	}
	return boc ? len - boc : len - cutoff;
}

/*
 * Return the position of the start of the patch or the length of str if there
 * is no patch in the message.
 */
static int find_patch_start(const char *str)
{
	const char *s;

	for (s = str; *s; s = next_line(s)) {
		if (starts_with(s, "---"))
			return s - str;
	}

	return s - str;
}

/*
 * Return the position of the first trailer line or len if there are no
 * trailers.
 */
static int find_trailer_start(const char *buf, size_t len)
{
	const char *s;
	int end_of_title, l, only_spaces = 1;
	int recognized_prefix = 0, trailer_lines = 0, non_trailer_lines = 0;
	/*
	 * Number of possible continuation lines encountered. This will be
	 * reset to 0 if we encounter a trailer (since those lines are to be
	 * considered continuations of that trailer), and added to
	 * non_trailer_lines if we encounter a non-trailer (since those lines
	 * are to be considered non-trailers).
	 */
	int possible_continuation_lines = 0;

	/* The first paragraph is the title and cannot be trailers */
	for (s = buf; s < buf + len; s = next_line(s)) {
		if (s[0] == comment_line_char)
			continue;
		if (is_blank_line(s))
			break;
	}
	end_of_title = s - buf;

	/*
	 * Get the start of the trailers by looking starting from the end for a
	 * blank line before a set of non-blank lines that (i) are all
	 * trailers, or (ii) contains at least one Git-generated trailer and
	 * consists of at least 25% trailers.
	 */
	for (l = last_line(buf, len);
	     l >= end_of_title;
	     l = last_line(buf, l)) {
		const char *bol = buf + l;
		const char *const *p;
		int separator_pos;

		if (bol[0] == comment_line_char) {
			non_trailer_lines += possible_continuation_lines;
			possible_continuation_lines = 0;
			continue;
		}
		if (is_blank_line(bol)) {
			if (only_spaces)
				continue;
			non_trailer_lines += possible_continuation_lines;
			if (recognized_prefix &&
			    trailer_lines * 3 >= non_trailer_lines)
				return next_line(bol) - buf;
			else if (trailer_lines && !non_trailer_lines)
				return next_line(bol) - buf;
			return len;
		}
		only_spaces = 0;

		for (p = git_generated_prefixes; *p; p++) {
			if (starts_with(bol, *p)) {
				trailer_lines++;
				possible_continuation_lines = 0;
				recognized_prefix = 1;
				goto continue_outer_loop;
			}
		}

		separator_pos = find_separator(bol, separators);
		if (separator_pos >= 1 && !isspace(bol[0])) {
			trailer_lines++;
			possible_continuation_lines = 0;
			if (recognized_prefix)
				continue;
		} else if (isspace(bol[0]))
			possible_continuation_lines++;
		else {
			non_trailer_lines++;
			non_trailer_lines += possible_continuation_lines;
			possible_continuation_lines = 0;
		}
continue_outer_loop:
		;
	}

	return len;
}

/* Return the position of the end of the trailers. */
static int find_trailer_end(const char *buf, size_t len)
{
	return len - ignore_non_trailer(buf, len);
}

static char *find_trailer(const char *message, size_t* len)
{
	size_t patch_start = find_patch_start(message);
	size_t trailer_end = find_trailer_end(message, patch_start);
	size_t trailer_start = find_trailer_start(message, trailer_end);

	size_t trailer_len = trailer_end - trailer_start;

	char *buffer = git__malloc(trailer_len + 1);
	memcpy(buffer, message + trailer_start, trailer_len);
	buffer[trailer_len] = 0;

	*len = trailer_len;

	return buffer;
}

enum trailer_state {
	S_START = 0,
	S_KEY = 1,
	S_KEY_WS = 2,
	S_SEP_WS = 3,
	S_VALUE = 4,
	S_VALUE_NL = 5,
	S_VALUE_END = 6,
	S_IGNORE = 7,
};

#define NEXT(st) { state = (st); ptr++; continue; }
#define GOTO(st) { state = (st); continue; }

int git_commit_trailers(git_commit *commit, void *opaque, git_commit_trailer_cb cb)
{
	enum trailer_state state = S_START;
	int rc = 0;
	char *ptr;
	char *key;
	char *value;

	size_t trailer_len;
	char *trailer = find_trailer(git_commit_message(commit), &trailer_len);

	for (ptr = trailer;;) {
		switch (state) {
			case S_START: {
				if (*ptr == 0) {
					goto ret;
				}

				key = ptr;
				GOTO(S_KEY);
			}
			case S_KEY: {
				if (*ptr == 0) {
					goto ret;
				}

				if (isalnum(*ptr) || *ptr == '-') {
					// legal key character
					NEXT(S_KEY);
				}

				if (*ptr == ' ' || *ptr == '\t') {
					// optional whitespace before separator
					*ptr = 0;
					NEXT(S_KEY_WS);
				}

				if (strchr(separators, *ptr)) {
					*ptr = 0;
					NEXT(S_SEP_WS);
				}

				// illegal character
				GOTO(S_IGNORE);
			}
			case S_KEY_WS: {
				if (*ptr == 0) {
					goto ret;
				}

				if (*ptr == ' ' || *ptr == '\t') {
					NEXT(S_KEY_WS);
				}

				if (strchr(separators, *ptr)) {
					NEXT(S_SEP_WS);
				}

				// illegal character
				GOTO(S_IGNORE);
			}
			case S_SEP_WS: {
				if (*ptr == 0) {
					goto ret;
				}

				if (*ptr == ' ' || *ptr == '\t') {
					NEXT(S_SEP_WS);
				}

				value = ptr;
				NEXT(S_VALUE);
			}
			case S_VALUE: {
				if (*ptr == 0) {
					GOTO(S_VALUE_END);
				}

				if (*ptr == '\n') {
					NEXT(S_VALUE_NL);
				}

				NEXT(S_VALUE);
			}
			case S_VALUE_NL: {
				if (*ptr == ' ') {
					// continuation;
					NEXT(S_VALUE);
				}

				ptr[-1] = 0;
				GOTO(S_VALUE_END);
			}
			case S_VALUE_END: {
				if ((rc = cb(opaque, key, value))) {
					goto ret;
				}

				key = NULL;
				value = NULL;

				GOTO(S_START);
			}
			case S_IGNORE: {
				if (*ptr == 0) {
					goto ret;
				}

				if (*ptr == '\n') {
					NEXT(S_START);
				}

				NEXT(S_IGNORE);
			}
		}
	}

ret:
	git__free(trailer);
	return rc;
}

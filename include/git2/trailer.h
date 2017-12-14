/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_trailer_h__
#define INCLUDE_git_trailer_h__

typedef int(*git_commit_trailer_cb)(void *, const char *key, const char *value);

GIT_EXTERN(int) git_commit_trailers(git_commit *commit, void *state, git_commit_trailer_cb cb);

#endif

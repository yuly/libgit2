#include <git2.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) if ((expr) != 0) { fprintf(stderr, "err: %s\n%s\n", giterr_last()->message, #expr); exit(1); }

int trailer_cb(void* opaque, const char* key, const char* value) {
	printf("key = {{{%s}}}; value = {{{%s}}}\n", key, value);
	return 0;
}

int main(int argc, const char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <repo> <sha>\n", argv[0]);
		return 1;
	}

	git_libgit2_init();

	git_repository *repo = NULL;
	CHECK(git_repository_open(&repo, argv[1]));

	git_oid oid;
	CHECK(git_oid_fromstr(&oid, argv[2]));

	git_commit *commit = NULL;
	CHECK(git_commit_lookup(&commit, repo, &oid));

	git_commit_trailers(commit, NULL, trailer_cb);
}

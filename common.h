#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <git2.h>
#include <fuse.h>

/*
	use the fact that git does not support FIFO filetype to
	register removed entries
*/

#define G_ISREMOVE(m) (((m) & S_IFMT) == S_IFIFO)

#define error(errno, ...)                                                      \
	do {                                                                       \
		fprintf(stderr, __VA_ARGS__);                                          \
		return errno;                                                          \
	} while (0)

#define debug(...) fprintf(stderr, __VA_ARGS__);

#define error_out(errno, ...)                                                  \
	do {                                                                       \
		retval = errno;                                                        \
		fprintf(stderr, __VA_ARGS__);                                          \
		goto out;                                                              \
	} while (0)

#define debug_out(errno, ...)                                                  \
	do {                                                                       \
		retval = errno;                                                        \
		fprintf(stderr, __VA_ARGS__);                                          \
		goto out;                                                              \
	} while (0)

typedef struct fuse_git_data {
	char *repo_path;
	char *branch_name;

	git_repository *repo;
} fuse_git_data;

enum {
	KEY_BRANCH,
	KEY_DEBUG,
	KEY_HELP
};

typedef enum {
	FUSE_GIT_FILE,
	FUSE_GIT_DIR,
	FUSE_GIT_BRANCH,
	FUSE_GIT_OID
} fuse_git_type;

typedef struct fuse_git_entry {
	fuse_git_type type;
	git_tree_entry *tree_entry;
	const char *last;

	union {
		git_tree *tree;
		git_blob *blob;
		git_branch_iterator *iter;
		char *oid;
	} object;
} fuse_git_entry;

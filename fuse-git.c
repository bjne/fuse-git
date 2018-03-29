#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "common.h"

bool enable_debug = 0;

int mkdir_p(const char *pathname, mode_t mode) {
	char path[PATH_MAX];

	if (strncpy(path, pathname, PATH_MAX) == NULL)
		return -1;

	for (char *p=strchr(path+1, '/'); p ; p=strchr(p+1, '/')) {
		*p = '\0';

		if (mkdir(path, mode) && errno != EEXIST)
			return -1;
	
		*p = '/';
	}

	return 0;
}

void fuse_git_entry_free(fuse_git_entry *e)
{
	switch (e->type) {
		case FUSE_GIT_BRANCH:
			if (e->object.iter)
				git_branch_iterator_free(e->object.iter);
			break;

		case FUSE_GIT_DIR:
			git_tree_free(e->object.tree);
			break;

		case FUSE_GIT_FILE:
			git_tree_entry_free(e->tree_entry);
			git_blob_free(e->object.blob);
			break;

		case FUSE_GIT_OID:
			return;
	}

	free(e);
}

int fuse_git_entry_get(fuse_git_entry **out, const char *path)
{
	git_object *object = NULL, **obj_ptr;
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *tree_entry = NULL;
	fuse_git_entry *e = NULL;
	fuse_git_data *d = (fuse_git_data *)(fuse_get_context()->private_data);
	char *branch = strdup(path + 1);
	char *real_path;
	int retval = 0;

	if((e = *out = calloc(1, sizeof(fuse_git_entry))) == NULL)
		error_out(-ENOMEM, "failed to allocate memory for entry\n");

	if (*branch == 0) {
		e->type = FUSE_GIT_BRANCH;
		tree = NULL;
		goto out;
	}

	if ((real_path = strchr(branch, '/')))
		*real_path++ = 0;

	// TODO: change to git_branch_lookup
	if (git_revparse_single(&object, d->repo, branch) < 0)
		error_out(-ENOENT, "cannot find branch: %s\n", branch);

	switch (git_object_type(object)) {
		case GIT_OBJ_COMMIT:
			commit = (git_commit *)object;

			if (git_commit_tree(&tree, commit) < 0)
				error_out(-ENOENT, "failed to get branch: %s\n", branch);

			break;
		case GIT_OBJ_TREE:
			tree = (git_tree *)object;
			break;

		default:
			error_out(-ENOENT, "unknown object type\n");
	}

	if (real_path == NULL) {
		e->type = FUSE_GIT_DIR;
		e->object.tree = tree;
		tree = NULL;
		goto out;
	}

	if (git_tree_entry_bypath(&tree_entry, tree, real_path) < 0)
		error_out(-ENOENT, "cannot find path: %s\n", real_path);

	switch (git_tree_entry_type(tree_entry)) {
		case GIT_OBJ_TREE:
			obj_ptr = (git_object **)&e->object.tree;
			e->type = FUSE_GIT_DIR;
			break;

		case GIT_OBJ_BLOB:
			obj_ptr = (git_object **)&e->object.blob;
			e->type = FUSE_GIT_FILE;
			e->tree_entry = tree_entry;
			break;

		case GIT_OBJ_COMMIT:
			debug_out(-ENOENT, "ignoring submodule entry: '%s'\n", real_path);

		default:
			debug_out(-ENOENT, "ignoring unknown entry: '%s'\n", real_path);
	}

	if (git_tree_entry_to_object(obj_ptr, d->repo, tree_entry) < 0)
		error_out(-EIO, "not found: %s\n", real_path);

	if (e->tree_entry)
		tree_entry = NULL;

out:
	if (retval && e) {
		fuse_git_entry_free(e);
		*out = NULL;
	}

	free(branch);
	git_tree_free(tree);
	git_tree_entry_free(tree_entry);
	git_object_free(object);

	return retval;
}

int fuse_git_branch_list(fuse_git_entry *e, fuse_git_data *d, void *buf,
                         fuse_fill_dir_t filler)
{
	git_reference *ref = NULL;
	git_branch_t type;
	int err;

	if (e->object.iter == NULL &&
	    git_branch_iterator_new(&e->object.iter, d->repo, GIT_BRANCH_LOCAL))
		error(-ENOENT, "failed to initiate branch iterator\n");

	if(e->last) {
		filler(buf, e->last, NULL, 0);
		free(&e->last);
		e->last = NULL;
	}

	while (!e->last && !(err = git_branch_next(&ref, &type, e->object.iter))) {
		if (filler(buf, git_reference_shorthand(ref), NULL, 0) == 1)
			e->last = strdup(git_reference_shorthand(ref));

		git_reference_free(ref);
	}

	return err == GIT_ITEROVER ? 0 : err;
}

const char *get_process_name(const pid_t pid)
{
	char *name = calloc(1024, sizeof(char));
	if (name) {
		sprintf(name, "/proc/%d/cmdline", pid);
		FILE *f = fopen(name, "r");
		if (f) {
			size_t size;

			size = fread(name, sizeof(char), 1024, f);
			if (size > 0 && name[size - 1] == '\n')
				name[size - 1] = '\0';

			fclose(f);
		}
	}

	return name;
}

int fuse_git_workdir(char **real_path, const char *path)
{
	fuse_git_data *d =(fuse_git_data *)(fuse_get_context()->private_data);
	int ret;

	ret = asprintf(real_path, "%s/fuse-git/%s", d->repo_path, path);
	if (ret < 0)
		return -errno;
	
	if (ret > PATH_MAX) {
		free(*real_path);
		return -ENAMETOOLONG;
	}

	return 0;
}

/*
	TODO: fix up stbuf.st_mode permission bits
*/

int fuse_git_workdir_getattr(const char *path, struct stat *stbuf)
{
	char *real_path = NULL;
	int ret, offset;

	if ((ret = fuse_git_workdir(&real_path, path)))
		return ret;

	offset = strlen(real_path) - strlen(path);

	for (char *p=strchr(real_path+1, '/'); p ; p=strchr(p+1, '/')) {
		if(p <= real_path + offset)
			continue;

		*p = '\0';
		if (stat(real_path, stbuf)) {
			free(real_path);
			return 1;
		}

		if (G_ISREMOVE(stbuf->st_mode)) {
			free(real_path);
			return -ENOENT;
		}

		*p = '/';
	}

	ret = stat(real_path, stbuf);

	free(real_path);

	if (ret)
		return 1;
		
	return G_ISREMOVE(stbuf->st_mode) ? -ENOENT : 0;
}

int fuse_git_getattr(const char *path, struct stat *stbuf)
{
//	fuse_git_data *d =(fuse_git_data *)(fuse_get_context()->private_data);
	fuse_git_entry *e = NULL;
//	git_commit *commit;
	int retval = 0;

	if ((retval = fuse_git_workdir_getattr(path, stbuf)) != 1)
		return retval;

	if ((retval = fuse_git_entry_get(&e, path)) < 0)
		goto out;

	memset(stbuf, 0, sizeof(struct stat));

	switch (e->type) {
		case FUSE_GIT_BRANCH:
		case FUSE_GIT_DIR:
			stbuf->st_nlink = 2;
			stbuf->st_mode = 040755;
			stbuf->st_size = 4096;
			break;

		case FUSE_GIT_FILE:
			stbuf->st_nlink = 1;
			stbuf->st_mode = git_tree_entry_filemode(e->tree_entry);

			if (S_ISLNK(stbuf->st_mode))
				stbuf->st_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;

			stbuf->st_size = git_blob_rawsize(e->object.blob);
			break;

		default:
			error_out(-EIO, "unsupported type\n");
	}

	/*
		warning: doint commit lookup could slow things down, benchmark!
	*/

	if (e->tree_entry == NULL)
		goto out;

	/*	TODO: figure out a way to make this work!
	if (git_commit_lookup(&commit, d->repo, git_tree_entry_id(e->tree_entry)))
		error_out(-EIO, "commit not found\n");

	stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = git_commit_time(commit);

	git_commit_free(commit);
	*/
out:
	if (e)
		fuse_git_entry_free(e);

	return retval;
}

int fuse_git_open(const char *path, struct fuse_file_info *fi)
{
	pid_t pid = (pid_t)(fuse_get_context()->pid);
//	struct stat stbuf;
//	if (fuse_tmp_getattr(path, &stbuf) || fi->flags == 1)

	printf("PID: %d\n", pid);
	printf("NAME: %s\n", get_process_name(pid));

	return fuse_git_entry_get((fuse_git_entry **)&fi->fh, path);
}

int fuse_git_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;

	if(fi->fh)
		fuse_git_entry_free((fuse_git_entry *)(intptr_t)fi->fh);

	return 0;
}

int fuse_git_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi)
{
	fuse_git_data *d = (fuse_git_data *)(fuse_get_context()->private_data);
	fuse_git_entry *e = (fuse_git_entry *)(intptr_t)fi->fh;
	const git_tree_entry *tree_entry;
	char *filename;
	int count;

	if (e->type == FUSE_GIT_BRANCH)
		return fuse_git_branch_list(e, d, buf, filler);

	if (e->type != FUSE_GIT_DIR)
		error(-EIO, "path is not a directory: '%s'\n", path);

	struct stat stbuf;
	const char *entry_name;
	for (count=git_tree_entrycount(e->object.tree);offset < count;offset++) {
		tree_entry = git_tree_entry_byindex(e->object.tree, offset);
		entry_name = git_tree_entry_name(tree_entry);

		if(asprintf(&filename, "%s/%s", path, entry_name) == -1)
			error(-ENOMEM, "failed to construct filename path");

		/*
			check if entry is present (or deleted) in workdir, and skip it here
		*/
		if (fuse_git_workdir_getattr(filename, &stbuf) != 1) {
			free(filename);
			continue;
		}

		if (filler(buf, entry_name, NULL, offset + 1) == 1) {
			free(filename);
			break;
		}
	}

	if (offset >= (off_t)git_tree_entrycount(e->object.tree)) {
		if(fuse_git_workdir_getattr(path, &stbuf) && !G_ISREMOVE(stbuf.st_mode)) {
		}
	}

	/*
		add items from workdir
	*/

	return 0;
}

void *fuse_git_init(struct fuse_conn_info *conn)
{
	(void)conn;
	return 0;
}

void fuse_git_destroy(void *private_data)
{
	(void)private_data;
	// TODO
}

int fuse_git_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi)
{
	fuse_git_entry *e = (fuse_git_entry *)(intptr_t)fi->fh;
	const void *blob;
	size_t blob_size;

	(void)path;

	switch (e->type) {
		case FUSE_GIT_FILE:
			if (!S_ISREG(git_tree_entry_filemode(e->tree_entry)))
				error(-EIO, "not a regular file\n");

			blob_size = git_blob_rawsize(e->object.blob);
			blob = git_blob_rawcontent(e->object.blob);
			break;

		case FUSE_GIT_OID:
			blob_size = GIT_OID_HEXSZ + 1;
			blob = e->object.oid;
			break;

		default:
			error(-EIO, "not a regular file\n");
	}

	if (offset >= (off_t)blob_size)
		size = 0;
	else if (offset + size > blob_size)
		size = blob_size - offset;

	if (size)
		memcpy(buf, blob + offset, size);

	return size;
}

//int add_blob(git_repository *repo, struct staging *stage, const char *file, const char *path)
//{
//	git_oid blob_id;
//	size_t len;
//	int retval = 0;
//
//	/*
//		Note: git_tree_create_updated (used in commit)
//		Deleting and adding the same entry is undefined behaviour
//	*/
//
//	/*
//		Todo: do not add blob if it already exists, or atleast keep
//		or atleast keep a state, so if we know if we should delete
//		it it any of the below actions fails
//	*/
//
//	if (git_blob_create_fromdisk(&blob_id, repo, file))
//		error_out(-1, "failed to add blob");
//
//	if (stage->n_updates % STAGE_ALLOC == 0) {
//		len = (stage->n_updates + STAGE_ALLOC) * sizeof(git_tree_update);
//		if ((stage->updates = realloc(stage->updates, len)) == NULL)
//			error_out(-ENOMEM, "failed to allocate memory for updates");
//
//		len = (stage->n_updates + STAGE_ALLOC) * sizeof(struct update_context);
//		if ((stage->context = realloc(stage->context, len)) == NULL)
//			error_out(-ENOMEM, "failed to allocate memory for context");
//	}
//
//	stage->updates[stage->n_updates] = (git_tree_update) {
//		.action = GIT_TREE_UPDATE_UPSERT,
//		.id = blob_id,
//		.filemode = GIT_FILEMODE_BLOB,
//		.path = strdup(path)
//	};
//
//	stage->context[stage->n_updates++] = (struct update_context) {
//		.name = "bjornar ness",
//		.email = "bjornar.ness@gmail.com",
//		.command = "vim"
//	};
//
//out:
//	/*
//		if (retval && blob_added)
//			delete_blob(blob_id);
//	*/
//
//	return retval;
//}

int fuse_git_write(const char *path, const char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
	int res;
	(void) path;

	if ((res = pwrite(fi->fh, buf, size, offset)) == -1)
		return -errno;

	return res;
}

/*
int fuse_git_readlink(const char *path, char *buf, size_t size)
{
}
*/

int fuse_git_mkdir(const char *path, mode_t mode)
{
	char p[PATH_MAX];
	fuse_git_data *d =(fuse_git_data *)(fuse_get_context()->private_data);
	int ret;

	if ((ret = snprintf(p, PATH_MAX, "%s/fuse-git/%s", d->repo_path, path)) < 0)
		error(-ENOMEM, "failed to allocate full path\n");

	if (ret >= PATH_MAX)
		error(-ENAMETOOLONG, "full path name too long");

	if (mkdir_p(p, 0755))
		return -1;

	return mkdir(p, mode);
}

struct fuse_operations fuse_git_operations = {
//	.init = fuse_git_init,
//	.destroy = fuse_git_destroy,
	.open = fuse_git_open,
	.release = fuse_git_release,
	.opendir = fuse_git_open,
	.releasedir = fuse_git_release,
	.getattr = fuse_git_getattr,
	.readdir = fuse_git_readdir,
	.read = fuse_git_read,
	.mkdir = fuse_git_mkdir,
//	.write = fuse_git_write
//	.readlink = fuse_git_readlink,
};

void fuse_git_usage(struct fuse_args *args, FILE *out)
{
	fprintf(out,
		"usage: %s [options] repository-path mountpoint\n"
		"\n",
		args->argv[0]);

	fuse_opt_add_arg(args, "-ho");
	fuse_main(args->argc, args->argv, &fuse_git_operations, NULL);
}

//int fuse_git_write()
//{
//	int git_blob_create_fromstream(git_writestream **out, git_repository *repo, const char *hintpath);
//}

/*
static int fuse_git_opt_proc(void *data, const char *arg, int key,
                             struct fuse_args *outargs)
{
	struct fuse_git_data *d = (struct fuse_git_data *)data;

	if (key == FUSE_OPT_KEY_NONOPT && d->repo_path == NULL) {
		if ((d->repo_path = realpath(arg, NULL)) == NULL)
			error(-1, "%s: failed to resolve path: %s\n", arg, strerror(errno));

		return 0;
	}

	switch (key) {
		case KEY_BRANCH:
			d->branch = strdup(strchr(arg, '=') + 1);
			return 0;

		case KEY_DEBUG:
			enable_debug = 1;
			return 1;

		case KEY_HELP:
			fuse_git_usage(outargs, stdout);
			exit(0);

		default:
			return 1;
	}
}
*/

int main(int argc, char *argv[])
{
	fuse_git_data *d = NULL;
//	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	int retval = 0;
	
//	if (fuse_opt_parse(&args);

	git_libgit2_init();


	if((d = calloc(1, sizeof(fuse_git_data))) == NULL)
		error_out(-ENOMEM, "failed to allocate memory for userdata\n");

	if ((d->repo_path = strndup("/usr/src/linux.git", PATH_MAX)) == NULL)
		error_out(-ENOMEM, "failed to allocate memory for repo_path\n");

	if (git_repository_open(&d->repo, d->repo_path) < 0)
		error_out(-1, "cannot open repository: %s\n", giterr_last()->message);

	fuse_main(argc, argv, &fuse_git_operations, d);
out:
//	fuse_opt_free_args(&args);

	if(d) {
		git_repository_free(d->repo);
		free(d->repo_path);
		free(d);
	}

	git_libgit2_shutdown();

	exit(retval);
}

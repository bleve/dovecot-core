/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "home-expand.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "subscription-file/subscription-file.h"
#include "mail-custom-flags.h"
#include "mbox-index.h"
#include "mbox-lock.h"
#include "mbox-storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define CREATE_MODE 0770 /* umask() should limit it more */

extern struct mail_storage mbox_storage;
extern struct mailbox mbox_mailbox;

static int mbox_handle_errors(struct mail_storage *storage)
{
	if (ENOACCESS(errno))
		mail_storage_set_error(storage, "Permission denied");
	else if (ENOSPACE(errno))
		mail_storage_set_error(storage, "Not enough disk space");
	else if (ENOTFOUND(errno))
		mail_storage_set_error(storage, "Directory structure is broken");
	else
		return FALSE;
	return TRUE;
}

static int mbox_autodetect(const char *data)
{
	const char *path;
	struct stat st;

	data = t_strcut(data, ':');

	/* Is it INBOX file? */
	if (*data != '\0' && stat(data, &st) == 0 && !S_ISDIR(st.st_mode) &&
	    access(data, R_OK|W_OK) == 0)
		return TRUE;

	/* or directory for IMAP folders? */
	path = t_strconcat(data, "/.imap", NULL);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
	    access(path, R_OK|W_OK|X_OK) == 0)
		return TRUE;

	path = t_strconcat(data, "/inbox", NULL);
	if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode) &&
	    access(path, R_OK|W_OK) == 0)
		return TRUE;

	path = t_strconcat(data, "/mbox", NULL);
	if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode) &&
	    access(path, R_OK|W_OK) == 0)
		return TRUE;

	return FALSE;
}

static const char *get_root_dir(void)
{
	const char *home, *path;

	if (mbox_autodetect(""))
		return "/";

	home = getenv("HOME");
	if (home != NULL) {
		path = t_strconcat(home, "/mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0)
			return path;

		path = t_strconcat(home, "/Mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0)
			return path;
	}

	return NULL;
}

static const char *get_inbox_file(const char *root_dir, int only_root)
{
	const char *user, *path;

	if (!only_root) {
		user = getenv("USER");
		if (user != NULL) {
			path = t_strconcat("/var/mail/", user, NULL);
			if (access(path, R_OK|W_OK) == 0)
				return path;

			path = t_strconcat("/var/spool/mail/", user, NULL);
			if (access(path, R_OK|W_OK) == 0)
				return path;
		}
	}

	return t_strconcat(root_dir, "/inbox", NULL);
}

static const char *create_root_dir(void)
{
	const char *home, *path;

	home = getenv("HOME");
	if (home == NULL) {
		i_error("mbox: We need root IMAP folder, "
			"but can't find it or HOME environment");
		return NULL;
	}

	path = t_strconcat(home, "/mail", NULL);
	if (mkdir_parents(path, CREATE_MODE) < 0) {
		i_error("mbox: Can't create root IMAP folder %s: %m", path);
		return NULL;
	}

	return path;
}

static struct mail_storage *mbox_create(const char *data, const char *user)
{
	struct mail_storage *storage;
	const char *root_dir, *inbox_file, *index_dir, *p;
	struct stat st;
	int autodetect;

	root_dir = inbox_file = index_dir = NULL;

	autodetect = data == NULL || *data == '\0';
	if (autodetect) {
		/* we'll need to figure out the mail location ourself.
		   it's root dir if we've already chroot()ed, otherwise
		   either $HOME/mail or $HOME/Mail */
		root_dir = get_root_dir();
	} else {
		/* <root folder> | <INBOX path>
		   [:INBOX=<path>] [:INDEX=<dir>] */
		p = strchr(data, ':');
		if (p == NULL) {
			if (stat(data, &st) < 0) {
				i_error("Invalid mbox file %s: %m", data);
				return NULL;
			}

			if (S_ISDIR(st.st_mode))
				root_dir = data;
			else {
				root_dir = get_root_dir();
				inbox_file = data;
			}
		} else {
			root_dir = t_strdup_until(data, p);
			do {
				p++;
				if (strncmp(p, "INBOX=", 6) == 0)
					inbox_file = t_strcut(p+6, ':');
				else if (strncmp(p, "INDEX=", 6) == 0)
					index_dir = t_strcut(p+6, ':');
				p = strchr(p, ':');
			} while (p != NULL);
		}
	}

	if (root_dir == NULL) {
		root_dir = create_root_dir();
		if (root_dir == NULL)
			return NULL;
	}

	if (inbox_file == NULL)
		inbox_file = get_inbox_file(root_dir, !autodetect);

	if (index_dir == NULL)
		index_dir = root_dir;
	else if (strcmp(index_dir, "MEMORY") == 0)
		index_dir = NULL;

	storage = i_new(struct mail_storage, 1);
	memcpy(storage, &mbox_storage, sizeof(struct mail_storage));

	storage->dir = i_strdup(home_expand(root_dir));
	storage->inbox_file = i_strdup(home_expand(inbox_file));
	storage->index_dir = i_strdup(home_expand(index_dir));
	storage->user = i_strdup(user);
	storage->callbacks = i_new(struct mail_storage_callbacks, 1);
	index_storage_init(storage);
	return storage;
}

static void mbox_free(struct mail_storage *storage)
{
	index_storage_deinit(storage);

	i_free(storage->dir);
	i_free(storage->inbox_file);
	i_free(storage->index_dir);
	i_free(storage->user);
	i_free(storage->error);
	i_free(storage->callbacks);
	i_free(storage);
}

int mbox_is_valid_mask(const char *mask)
{
	const char *p;
	int newdir;

	if (full_filesystem_access)
		return TRUE;

	/* make sure it's not absolute path */
	if (*mask == '/' || *mask == '\\' || *mask == '~')
		return FALSE;

	/* make sure there's no "../" or "..\" stuff */
	newdir = TRUE;
	for (p = mask; *p != '\0'; p++) {
		if (newdir && p[0] == '.' && p[1] == '.' &&
		    (p[2] == '/' || p[2] == '\\'))
			return FALSE;
		newdir = p[0] == '/' || p[0] == '\\';
	}

	return TRUE;
}

static int mbox_is_valid_create_name(struct mail_storage *storage,
				     const char *name)
{
	if (name[0] == '\0' || name[strlen(name)-1] == storage->hierarchy_sep ||
	    strchr(name, '*') != NULL || strchr(name, '%') != NULL)
		return FALSE;

	return mbox_is_valid_mask(name);
}

static int mbox_is_valid_existing_name(const char *name)
{
	if (name[0] == '\0')
		return FALSE;

	return mbox_is_valid_mask(name);
}

static const char *mbox_get_index_dir(struct mail_storage *storage,
				      const char *name)
{
	const char *p;

	if (storage->index_dir == NULL)
		return NULL;

	if (full_filesystem_access && (*name == '/' || *name == '~')) {
		name = home_expand(name);
		p = strrchr(name, '/');
		return t_strconcat(t_strdup_until(name, p),
				   "/.imap/", p+1, NULL);
	}

	p = strrchr(name, '/');
	if (p == NULL)
		return t_strconcat(storage->index_dir, "/.imap/", name, NULL);
	else {
		return t_strconcat(storage->index_dir, "/",
				   t_strdup_until(name, p),
				   "/.imap/", p+1, NULL);
	}
}

static int create_mbox_index_dirs(struct mail_storage *storage,
				  const char *name)
{
	const char *index_dir;

	index_dir = mbox_get_index_dir(storage, name);
	if (index_dir == NULL)
		return TRUE;

	if (mkdir_parents(index_dir, CREATE_MODE) < 0) {
		mail_storage_set_critical(storage,
			"mkdir_parents(%s) failed: %m", index_dir);
		return FALSE;
	}

	return TRUE;
}

static int verify_inbox(struct mail_storage *storage)
{
	int fd;

	/* make sure inbox file itself exists */
	fd = open(storage->inbox_file, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1)
		(void)close(fd);

	/* make sure the index directories exist */
	if (!create_mbox_index_dirs(storage, "INBOX"))
		return FALSE;

	return TRUE;
}

static const char *mbox_get_path(struct mail_storage *storage, const char *name)
{
	if (strcasecmp(name, "INBOX") == 0)
		return storage->inbox_file;
	if (full_filesystem_access && (*name == '/' || *name == '~'))
		return home_expand(name);
	return t_strconcat(storage->dir, "/", name, NULL);
}

static struct mailbox *mbox_open(struct mail_storage *storage, const char *name,
				 enum mailbox_open_flags flags)
{
	struct index_mailbox *ibox;
	struct mail_index *index;
	const char *path, *index_dir;

	if (strcasecmp(name, "INBOX") == 0) {
		/* name = "INBOX"
		   path = "<inbox_file>/INBOX"
		   index_dir = "/mail/.imap/INBOX" */
		path = storage->inbox_file;
		index_dir = mbox_get_index_dir(storage, "INBOX");
	} else {
		/* name = "foo/bar"
		   path = "/mail/foo/bar"
		   index_dir = "/mail/foo/.imap/bar" */
		path = mbox_get_path(storage, name);
		index_dir = mbox_get_index_dir(storage, name);
	}

	index = index_storage_lookup_ref(index_dir);
	if (index == NULL) {
		index = mbox_index_alloc(path, index_dir, index_dir);
		index_storage_add(index);
	}

	ibox = index_storage_mailbox_init(storage, &mbox_mailbox, index,
					  name, flags);
	if (ibox != NULL)
		ibox->expunge_locked = mbox_expunge_locked;
	return (struct mailbox *) ibox;
}

static struct mailbox *
mbox_open_mailbox(struct mail_storage *storage,
		  const char *name, enum mailbox_open_flags flags)
{
	const char *path;
	struct stat st;

	mail_storage_clear_error(storage);

	/* INBOX is always case-insensitive */
	if (strcasecmp(name, "INBOX") == 0) {
		/* make sure inbox exists */
		if (!verify_inbox(storage))
			return FALSE;
		return mbox_open(storage, "INBOX", flags);
	}

	if (!mbox_is_valid_existing_name(name)) {
		mail_storage_set_error(storage, "Invalid mailbox name");
		return FALSE;
	}

	path = mbox_get_path(storage, name);
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			mail_storage_set_error(storage,
				"Mailbox isn't selectable: %s", name);
			return NULL;
		}

		/* exists - make sure the required directories are also there */
		if (!create_mbox_index_dirs(storage, name))
			return NULL;

		return mbox_open(storage, name, flags);
	}

	if (ENOTFOUND(errno)) {
		mail_storage_set_error(storage, "Mailbox doesn't exist: %s",
				       name);
	} else if (!mbox_handle_errors(storage))
		mail_storage_set_critical(storage, "stat(%s) failed: %m", path);

	return NULL;
}

static int mbox_create_mailbox(struct mail_storage *storage, const char *name,
			       int only_hierarchy)
{
	const char *path, *p;
	struct stat st;
	int fd;

	mail_storage_clear_error(storage);

	if (strcasecmp(name, "INBOX") == 0)
		name = "INBOX";

	if (!mbox_is_valid_create_name(storage, name)) {
		mail_storage_set_error(storage, "Invalid mailbox name");
		return FALSE;
	}

	/* make sure it doesn't exist already */
	path = mbox_get_path(storage, name);
	if (stat(path, &st) == 0) {
		mail_storage_set_error(storage, "Mailbox already exists");
		return FALSE;
	}

	if (errno != ENOENT && errno != ELOOP && errno != EACCES) {
		if (errno == ENOTDIR) {
			mail_storage_set_error(storage,
				"Mailbox doesn't allow inferior mailboxes");
		} else {
			mail_storage_set_critical(storage,
				"stat() failed for mbox file %s: %m", path);
		}
		return FALSE;
	}

	/* create the hierarchy if needed */
	p = only_hierarchy ? path + strlen(path) : strrchr(path, '/');
	if (p != NULL) {
		p = t_strdup_until(path, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return FALSE;

			mail_storage_set_critical(storage,
				"mkdir_parents(%s) failed: %m", p);
			return FALSE;
		}

		if (only_hierarchy) {
			/* wanted to create only the directory */
			return TRUE;
		}
	}

	/* create the mailbox file */
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1) {
		(void)close(fd);
		return TRUE;
	}

	if (errno == EEXIST) {
		/* mailbox was just created between stat() and open() call.. */
		mail_storage_set_error(storage, "Mailbox already exists");
	} else if (!mbox_handle_errors(storage)) {
		mail_storage_set_critical(storage,
			"Can't create mailbox %s: %m", name);
	}
	return FALSE;
}

static int mbox_delete_mailbox(struct mail_storage *storage, const char *name)
{
	const char *index_dir, *path;
	struct stat st;

	mail_storage_clear_error(storage);

	if (strcasecmp(name, "INBOX") == 0) {
		mail_storage_set_error(storage, "INBOX can't be deleted.");
		return FALSE;
	}

	if (!mbox_is_valid_existing_name(name)) {
		mail_storage_set_error(storage, "Invalid mailbox name");
		return FALSE;
	}

	path = mbox_get_path(storage, name);
	if (lstat(path, &st) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(storage,
				"Mailbox doesn't exist: %s", name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(storage, "lstat() failed for "
						  "%s: %m", path);
		}
		return FALSE;
	}

	if (S_ISDIR(st.st_mode)) {
		/* deleting a folder, only allow it if it's empty.
		   Delete .imap folder before to make sure it goes empty. */
		index_dir = t_strconcat(storage->index_dir, "/", name,
					"/.imap", NULL);

		if (index_dir != NULL && rmdir(index_dir) < 0 &&
		    !ENOTFOUND(errno) && errno != ENOTEMPTY) {
			if (!mbox_handle_errors(storage)) {
				mail_storage_set_critical(storage,
					"rmdir() failed for %s: %m", index_dir);
				return FALSE;
			}
		}

		if (rmdir(path) == 0)
			return TRUE;

		if (ENOTFOUND(errno)) {
			mail_storage_set_error(storage,
				"Mailbox doesn't exist: %s", name);
		} else if (errno == ENOTEMPTY) {
			mail_storage_set_error(storage,
				"Folder %s isn't empty, can't delete it.",
				name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(storage,
				"rmdir() failed for %s: %m", path);
		}
		return FALSE;
	}

	/* first unlink the mbox file */
	if (unlink(path) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(storage,
				"Mailbox doesn't exist: %s", name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(storage,
				"unlink() failed for %s: %m", path);
		}
		return FALSE;
	}

	/* next delete the index directory */
	index_dir = mbox_get_index_dir(storage, name);
	if (index_dir != NULL) {
		index_storage_destroy_unrefed();

		if (unlink_directory(index_dir, TRUE) < 0 && errno != ENOENT) {
			mail_storage_set_critical(storage,
				"unlink_directory(%s) failed: %m", index_dir);

			/* mailbox itself is deleted, so return success
			   anyway */
		}
	}

	return TRUE;
}

static int mbox_rename_mailbox(struct mail_storage *storage,
			       const char *oldname, const char *newname)
{
	const char *oldpath, *newpath, *old_indexdir, *new_indexdir, *p;
	struct stat st;

	mail_storage_clear_error(storage);

	if (!mbox_is_valid_existing_name(oldname) ||
	    !mbox_is_valid_create_name(storage, newname)) {
		mail_storage_set_error(storage, "Invalid mailbox name");
		return FALSE;
	}

	if (strcasecmp(oldname, "INBOX") == 0)
		oldname = "INBOX";

	oldpath = mbox_get_path(storage, oldname);
	newpath = mbox_get_path(storage, newname);

	/* create the hierarchy */
	p = strrchr(newpath, '/');
	if (p != NULL) {
		p = t_strdup_until(newpath, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return FALSE;

			mail_storage_set_critical(storage,
				"mkdir_parents(%s) failed: %m", p);
			return FALSE;
		}
	}

	/* first check that the destination mailbox doesn't exist.
	   this is racy, but we need to be atomic and there's hardly any
	   possibility that someone actually tries to rename two mailboxes
	   to same new one */
	if (lstat(newpath, &st) == 0) {
		mail_storage_set_error(storage,
				       "Target mailbox already exists");
		return FALSE;
	} else if (!ENOTFOUND(errno) && errno != EACCES) {
		mail_storage_set_critical(storage, "lstat(%s) failed: %m",
					  newpath);
		return FALSE;
	}

	/* NOTE: renaming INBOX works just fine with us, it's simply recreated
	   the next time it's needed. */
	if (rename(oldpath, newpath) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(storage,
				"Mailbox doesn't exist: %s", oldname);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(storage,
				"rename(%s, %s) failed: %m", oldpath, newpath);
		}
		return FALSE;
	}

	/* we need to rename the index directory as well */
	old_indexdir = mbox_get_index_dir(storage, oldname);
	new_indexdir = mbox_get_index_dir(storage, newname);
	if (old_indexdir != NULL) {
		if (rename(old_indexdir, new_indexdir) < 0) {
			mail_storage_set_critical(storage,
						  "rename(%s, %s) failed: %m",
						  old_indexdir, new_indexdir);
		}
	}

	return TRUE;
}

static int mbox_get_mailbox_name_status(struct mail_storage *storage,
					const char *name,
					enum mailbox_name_status *status)
{
	struct stat st;
	const char *path;

	mail_storage_clear_error(storage);

	if (strcasecmp(name, "INBOX") == 0)
		name = "INBOX";

	if (!mbox_is_valid_existing_name(name)) {
		*status = MAILBOX_NAME_INVALID;
		return TRUE;
	}

	path = mbox_get_path(storage, name);
	if (stat(path, &st) == 0) {
		*status = MAILBOX_NAME_EXISTS;
		return TRUE;
	}

	if (!mbox_is_valid_create_name(storage, name)) {
		*status = MAILBOX_NAME_INVALID;
		return TRUE;
	}

	if (ENOTFOUND(errno) || errno == EACCES) {
		*status = MAILBOX_NAME_VALID;
		return TRUE;
	} else if (errno == ENOTDIR) {
		*status = MAILBOX_NAME_NOINFERIORS;
		return TRUE;
	} else {
		mail_storage_set_critical(storage, "mailbox name status: "
					  "stat(%s) failed: %m", path);
		return FALSE;
	}
}

static int mbox_storage_close(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;
	int failed = FALSE;

	/* update flags by rewrite mbox file */
        index_storage_init_lock_notify(ibox);
	if (!ibox->index->mailbox_readonly) {
		if (!mbox_index_rewrite(ibox->index)) {
			mail_storage_set_index_error(ibox);
			failed = TRUE;
		}
	}
	ibox->index->set_lock_notify_callback(ibox->index, NULL, NULL);

	return index_storage_mailbox_free(box) && !failed;
}

static void mbox_storage_auto_sync(struct mailbox *box,
				   enum mailbox_sync_type sync_type,
				   unsigned int min_newmail_notify_interval)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	ibox->autosync_type = sync_type;
	ibox->min_newmail_notify_interval = min_newmail_notify_interval;

	index_mailbox_check_remove_all(ibox);
	if (sync_type != MAILBOX_SYNC_NONE)
		index_mailbox_check_add(ibox, ibox->index->mailbox_path);
}

static int mbox_storage_lock(struct mailbox *box,
			     enum mailbox_lock_type lock_type)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	if (lock_type == MAIL_LOCK_UNLOCK) {
		ibox->lock_type = MAIL_LOCK_UNLOCK;
		if (!index_storage_lock(ibox, MAIL_LOCK_UNLOCK))
			return FALSE;
		return TRUE;
	}

	i_assert(ibox->lock_type == MAIL_LOCK_UNLOCK);

	if ((lock_type & (MAILBOX_LOCK_EXPUNGE | MAILBOX_LOCK_FLAGS)) != 0) {
		if (!index_storage_lock(ibox, MAIL_LOCK_EXCLUSIVE))
			return FALSE;
	} else if ((lock_type & MAILBOX_LOCK_READ) != 0) {
		if (!index_storage_lock(ibox, MAIL_LOCK_SHARED))
			return FALSE;
	}

	if ((lock_type & (MAILBOX_LOCK_EXPUNGE | MAILBOX_LOCK_SAVE)) != 0) {
		/* FIXME: saving doesn't have to sync it, just lock it */
		if (!index_storage_sync_and_lock(ibox, FALSE, TRUE,
						 MAIL_LOCK_EXCLUSIVE))
			return FALSE;
	}

	ibox->lock_type = lock_type;
	return TRUE;
}

struct mail_storage mbox_storage = {
	"mbox", /* name */

	'/', /* hierarchy_sep - can't be changed */

	mbox_create,
	mbox_free,
	mbox_autodetect,
	index_storage_set_callbacks,
	mbox_open_mailbox,
	mbox_create_mailbox,
	mbox_delete_mailbox,
	mbox_rename_mailbox,
	mbox_list_mailbox_init,
	mbox_list_mailbox_deinit,
	mbox_list_mailbox_next,
	subsfile_set_subscribed,
	mbox_get_mailbox_name_status,
	mail_storage_get_last_error,

	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL, NULL, NULL,

	0
};

struct mailbox mbox_mailbox = {
	NULL, /* name */
	NULL, /* storage */

	mbox_storage_close,
	mbox_storage_lock,
	index_storage_get_status,
	index_storage_sync,
	mbox_storage_auto_sync,
	index_storage_expunge,
	index_storage_fetch_init,
	index_storage_fetch_deinit,
	index_storage_fetch_next,
	index_storage_fetch_uid,
	index_storage_fetch_seq,
        index_storage_search_get_sorting,
	index_storage_search_init,
	index_storage_search_deinit,
	index_storage_search_next,
	mbox_storage_save_init,
	mbox_storage_save_deinit,
	mbox_storage_save_next,
	index_storage_copy_init,
	index_storage_copy_deinit,
	mail_storage_is_inconsistency_error,

	FALSE,
	FALSE,
	FALSE
};

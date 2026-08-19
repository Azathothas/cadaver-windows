/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2008, Joe Orton <joe@manyfish.co.uk>, 

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
  
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#include <stdio.h>
#include <fcntl.h>

#include <ne_basic.h>
#include <ne_alloc.h>
#include <ne_string.h>

#include "system.h"
#include "cadaver.h"
#include "commands.h"
#include "utils.h"
#include "options.h"
#include "i18n.h"

static int run_editor(const char *filename)
{
    char *editcmd;
    const char *editor;
    struct cad_finfo before, after;
    int ret;

    /* $VISUAL, $EDITOR and the platform default are all
     * cad_default_editor()'s business; the option set inside cadaver
     * wins over any of them. */
    editor = get_option(opt_editor);
    if (editor == NULL) editor = cad_default_editor();

    /* The temporary directory routinely has a space in it on Windows,
     * so the file name is quoted; the editor is not, because it may
     * legitimately carry options of its own. */
    editcmd = cad_command_with_path(editor, filename);

    if (cad_file_info(filename, &before)) {
	out_printf(_("edit: Could not stat file: %s\n"), strerror(errno));
        cmd_failed(strerror(errno));
	ne_free(editcmd);
	return -1;
    }
    out_printf(_("edit: Running editor: `%s'...\n"), editcmd);
    ret = system(editcmd);
    if (ret == -1) {
	out_printf(_("edit: Error executing editor: %s\n"), strerror(errno));
        cmd_failed(strerror(errno));
    }
    ne_free(editcmd);
    if (cad_file_info(filename, &after)) {
        cmd_failed(strerror(errno));
        out_printf(_("edit: Error: Could not examine temporary file: %s\n"),
               strerror(errno));
        return -1;
    }
    /* The size as well as the timestamp: an edit made and saved inside
     * the same second is common with a fast editor and a small file,
     * and a one-second timestamp would call that no change. */
    if (before.mtime == after.mtime && before.size == after.size) {
	/* File not changed. */
	out_printf(_("edit: No changes were made.\n"));
	return -1;
    } else {
	out_printf(_("edit: Changes were made.\n"));
	return 0;
    }	
}

/* Returns true if resource at URI is lockable. */
static int is_lockable(const char *uri_path)
{
    ne_server_capabilities caps = {0};
    ne_uri uri = session.uri; /* shallow copy */

    uri.path = (char *)uri_path;

    if (ne_lockstore_findbyuri(session.locks, &uri) != NULL)
        return 0;

    /* A proper test for "lockability" would be to check the
     * supportedlock property here, but this is sufficient. */
    if (ne_options(session.sess, uri_path, &caps) != NE_OK) {
	return 0;
    }

    return !!caps.dav_class2;
}

/* Pre-send hook to use a conditional PUT. */
static void edit_pre_send(ne_request *req, void *userdata, ne_buffer *header)
{
    char *etag = userdata;

    if (etag) ne_buffer_concat(header, "If-Match: ", etag, "\r\n", NULL);
}

/* Post-headers hook to fetch the etag for GET. */
static void edit_hdrs(ne_request *req, void *userdata, const ne_status *status)
{
    char **etag = userdata;
    const char *val;

    if (status->klass == 2
        && (val = ne_get_response_header(req, "Etag")) != NULL) {
        if (*etag) ne_free(*etag);
        *etag = ne_strclean(ne_strdup(val));
    }
}

/* Post-send hook to produce a descriptive failure for a conditional
 * PUT error. */
static int edit_post_send(ne_request *req, void *userdata, const ne_status *status)
{
    if (status->code == 412) {
        ne_set_error(session.sess, _("Resource was modified since download, "
                                     "upload refused"));
        return NE_ERROR;
    }

    return NE_OK;
}

void execute_edit(const char *native_path)
{
    char *uri_path, *etag = NULL, *fname = NULL;
    struct ne_lock *lock = NULL;
    const char *pnt;
    int fd;
    int is_checkout, is_checkin, can_lock;

    /* The editor is a program that inherits standard output, which
     * with --json carries the result document.  `cat' and `less' are
     * refused for the same reason. */
    if (out_json) {
        out_start(_("Editing"), native_path);
        out_fail(_("`edit' runs an editor, which writes to standard output; "
                   "with --json that carries the result document. "
                   "Use `get' and `put'.\n"));
        return;
    }

    uri_path = uri_resolve_native(native_path);

    /* The getrestype()  -> PROPFIND
     *     is_lockable() -> OPTIONS
     *     is_vcr()      -> PROPFIND
     * sequence could be condensed into one PROPFIND for the right set
     * of properties here (resourcetype, checked-in, supportedlock).
     *
     * Prevent edit on a collection. Following a (custom?)property to
     * retrieve "index.html" for a collection would be a possible RFE
     * here. */
    if (getrestype(uri_path) == resr_collection) {
        cmd_failed(_("that is a collection"));
	out_printf(_("You cannot edit a collection resource (%s).\n"),
	       uri_path);
	goto edit_bail;
    }

    can_lock = is_lockable(uri_path);

    /* Give the local temp file the same extension as the remote path,
     * so the editor can have a stab at the content-type. */
    pnt = strrchr(uri_path, '.');
    if (pnt != NULL && strchr(pnt, '/') != NULL) pnt = NULL;

    fname = ne_concat(cad_tmp_dir(), CAD_DIR_SEPARATOR "cadaver-edit-XXXXXX",
		      pnt ? pnt : "", NULL);

    /* cad_mkstemp() replaces the XXXXXX in place, wherever it falls in
     * the name, and creates the file with O_EXCL and mode 0600.  There
     * is no separate chmod afterwards: a file that has only ever
     * existed with those permissions cannot be read in the window
     * before one. */
    fd = cad_mkstemp(fname);
    if (fd == -1) {
	out_printf(_("Could not create temporary file %s:\n%s\n"), fname,
	       strerror(errno));
        cmd_failed(strerror(errno));
	goto edit_bail;
    }

    if (can_lock) {
	lock = ne_lock_create();
	ne_fill_server_uri(session.sess, &lock->uri);
	lock->uri.path = ne_strdup(uri_path);
	lock->owner = getowner();
	out_start_uri(_("Locking"), uri_path);
	if (out_handle(ne_lock(session.sess, lock))) {
	    ne_lockstore_add(session.locks, lock);
	} else {
	    ne_lock_destroy(lock);
	    goto edit_close;
	}
    }

    /* Return 1: Checkin, 2: Checkout, 0: otherwise */
    if ((is_checkin = is_vcr(uri_path)) == 1) {
        execute_checkout(uri_path);
    }

    ne_hook_post_headers(session.sess, edit_hdrs, &etag);
    output(o_download, _("Downloading `%s' to %s"), uri_path, fname);
    /* Don't puke if get fails -- perhaps we are creating a new one? */
    out_result(ne_get(session.sess, uri_path, fd));

    ne_unhook_post_headers(session.sess, edit_hdrs, &etag);

    if (close(fd)) {
	out_fail(_("error writing to temporary file: %s\n"), 
	       strerror(errno));
    } 
    else if (!run_editor(fname)) {
	int upload_okay = 0;

	fd = open(fname, O_RDONLY | OPEN_BINARY_FLAGS);
	if (fd < 0) {
	    output(o_finish, 
		   _("Could not re-open temporary file: %s\n"),
		   strerror(errno));
	} else {
            if (etag) {
                ne_hook_pre_send(session.sess, edit_pre_send, etag);
                ne_hook_post_send(session.sess, edit_post_send, NULL);
            }

            do {
		output(o_upload, _("Uploading changes to `%s'"), uri_path);

		if (out_handle(ne_put(session.sess, uri_path, fd))) {
		    upload_okay = 1;
		} else {
		    /* TODO: offer to save locally instead */
		    out_printf(_("Try uploading again (y/n)? "));
		    if (!yesno()) {
			upload_okay = 1;
		    }
		}
	    } while (!upload_okay);

            if (etag) {
                ne_unhook_pre_send(session.sess, edit_pre_send, etag);
                ne_unhook_post_send(session.sess, edit_post_send, NULL);
            }
	    close(fd);
	}
    }
    
    if (unlink(fname)) {
        cmd_failed(strerror(errno));
	out_printf(_("edit: Could not delete temporary file %s:\n%s\n"), fname,
	       strerror(errno));
    }	       

    /* Return 1: Checkin, 2: Checkout, 0: otherwise */
    is_checkout = is_vcr(uri_path);
    if (is_checkout==2) {
        execute_checkin(uri_path);
    }
    
    /* UNLOCK it again whether we succeed or failed in our mission */
    if (can_lock) {
	out_start_uri(_("Unlocking"), uri_path);
	out_result(ne_unlock(session.sess, lock));
	ne_lockstore_remove(session.locks, lock);
	ne_lock_destroy(lock);
    }

    if (etag) ne_free(etag);

    goto edit_bail;
edit_close:
    close(fd);
edit_bail:
    if (fname) ne_free(fname);
    ne_free(uri_path);
    return;
}

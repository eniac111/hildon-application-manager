/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2005, 2006, 2007 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <libintl.h>

#include <gtk/gtk.h>
#include <glib/gspawn.h>

#include "log.h"
#include "util.h"
#include "settings.h"
#include "apt-worker-client.h"
#include "apt-worker-proto.h"

#define _(x) gettext (x)

int apt_worker_out_fd = -1;
int apt_worker_in_fd = -1;
int apt_worker_cancel_fd = -1;
int apt_worker_status_fd = -1;

/* if apt-worker has started up properly */
gboolean apt_worker_started = FALSE;

/* apt-worker start timeout in miliseconds. If apt-worker doesn't start in this time,
 * then an error is returned. */
#define APT_WORKER_START_TIMEOUT 3000

static void cancel_request (int cmd);
static void cancel_all_pending_requests ();

static bool status_out_of_space = false;

void
reset_client_error_status ()
{
  status_out_of_space = false;
}

bool
client_error_out_of_space ()
{
  return status_out_of_space;
}

static GString *pmstatus_line;

static void
interpret_pmstatus (char *str)
{
  float percentage;
  char *title;

  if (!strncmp (str, "pmstatus:", 9))
    {
      str += 9;
      str = strchr (str, ':');
      if (str == NULL)
	return;
      str += 1;
      percentage = atof (str);
      str = strchr (str, ':');
      if (str == NULL)
	title = "Working";
      else
	{
	  str += 1;
	  title = str;
	}
	
      set_progress (op_general, (int)percentage, 100);
    }
}

static gboolean
read_pmstatus (GIOChannel *channel, GIOCondition cond, gpointer data)
{
  char buf[256], *line_end;
  int n, fd = g_io_channel_unix_get_fd (channel);

  n = read (fd, buf, 256);
  if (n > 0)
    {
      g_string_append_len (pmstatus_line, buf, n);
      while ((line_end = strchr (pmstatus_line->str, '\n')))
	{
	  *line_end = '\0';
	  interpret_pmstatus (pmstatus_line->str);
	  g_string_erase (pmstatus_line, 0, line_end - pmstatus_line->str + 1);
	}
      return TRUE;
    }
  else
    {
      g_io_channel_shutdown (channel, 0, NULL);
      return FALSE;
    }
}

static void
setup_pmstatus_from_fd (int fd)
{
  pmstatus_line = g_string_new ("");
  GIOChannel *channel = g_io_channel_unix_new (fd);
  g_io_add_watch (channel, GIOCondition (G_IO_IN | G_IO_HUP | G_IO_ERR),
		  read_pmstatus, NULL);
  g_io_channel_unref (channel);
}

static bool
must_mkfifo (char *filename, int mode)
{
  if (unlink (filename) < 0 && errno != ENOENT)
    log_perror (filename);
    
  if (mkfifo (filename, mode) < 0)
    {
      log_perror (filename);
      return false;
    }
  return true;
}

static bool
must_unlink (char *filename)
{
  if (unlink (filename) < 0)
    {
      log_perror (filename);
      return false;
    }
  return true;
}

static int
must_open (char *filename, int flags)
{
  int fd = open (filename, flags);
  int arg;
  if (fd < 0)
    {
      log_perror (filename);
      return -1;
    }
  arg = fcntl (fd, F_GETFL, NULL);
  arg ^= O_NONBLOCK;
  fcntl (fd, F_SETFL, arg);
  return fd;
}

struct try_apt_worker_closure
{
  /* current step of apt-worker file opening init */
  int start_step;
  apt_worker_start_callback *finished_cb;
  void *finished_data;
  apt_worker_start_callback_tick *tick_cb;
  void *tick_data;
  /* id of the timeout GSource */
  guint timeout_id;
  /* time elapsed since start_apt_worker has been called */
  guint rounds_passed;
  int stdout_fd;
  int stderr_fd;
};

try_apt_worker_closure *start_closure = NULL;

void
cancel_apt_worker_start ()
{
  if (start_closure != NULL)
    {
      if (start_closure->timeout_id != 0)
	{
	  g_source_remove (start_closure->timeout_id);
	  start_closure->timeout_id = 0;
	}
      if (start_closure->finished_cb)
	start_closure->finished_cb (FALSE, start_closure->finished_data);
      delete start_closure;
      start_closure = NULL;
    }
}

static gboolean
try_apt_worker_start (void *data)
{
  try_apt_worker_closure *closure = (try_apt_worker_closure *) data;
  gboolean end_loop = FALSE;

  /* Iterates to open the files in the proper order */
  while (!end_loop && closure->start_step < 4)
    {
      switch (closure->start_step)
	{
	case 0:
	  if ((apt_worker_out_fd = must_open ("/tmp/apt-worker.to", O_WRONLY|O_NONBLOCK)) >= 0)
	    closure->start_step++;
	  else
	    end_loop = TRUE;
	  break;
	case 1:
	  if ((apt_worker_in_fd = must_open ("/tmp/apt-worker.from", O_RDONLY|O_NONBLOCK)) >= 0)
	    closure->start_step++;
	  else
	    end_loop = TRUE;
	  break;
	case 2:
	  if ((apt_worker_status_fd = must_open ("/tmp/apt-worker.status", O_RDONLY|O_NONBLOCK)) >= 0)
	    closure->start_step++;
	  else
	    end_loop = TRUE;
	  break;
	case 3:
	  if ((apt_worker_cancel_fd = must_open ("/tmp/apt-worker.cancel", O_WRONLY|O_NONBLOCK)) >= 0)
	    closure->start_step++;
	  else
	    end_loop = TRUE;
	  break;
	default:
	  break;
	}
    }
  /* If all init steps were done, then call the finish callback and finish initialisation */
  if (closure->start_step == 4)
    {

      must_unlink ("/tmp/apt-worker.to");
      must_unlink ("/tmp/apt-worker.from");
      must_unlink ("/tmp/apt-worker.status");
      must_unlink ("/tmp/apt-worker.cancel");

      log_from_fd (closure->stdout_fd);
      log_from_fd (closure->stderr_fd);
      setup_pmstatus_from_fd (apt_worker_status_fd);
      apt_worker_started = TRUE;
      if (closure->finished_cb)
	closure->finished_cb (TRUE, closure->finished_data);
      delete closure;
      return FALSE;
    }
  /* setup timeout if it wasn't set up previously */
  else if (closure->timeout_id == 0)
    {
      closure->timeout_id = g_timeout_add (100, try_apt_worker_start, data);
      return FALSE;
    }
  else
    {
      /* If too much time has passed, end the attempts */
      closure->rounds_passed += 100;
      if (closure->rounds_passed > APT_WORKER_START_TIMEOUT)
	{
	  if (closure->finished_cb)
	    closure->finished_cb (FALSE, closure->finished_data);
	  delete closure;
	  return FALSE;
	}
    }

  /* call the ticker function (can be used for progress bars) */
  if (closure->tick_cb)
    closure->tick_cb (closure->tick_data);

  return TRUE;	
}

bool
start_apt_worker (gchar *prog, apt_worker_start_callback *finished_cb, void *finished_data,
		  apt_worker_start_callback_tick *tick_cb, void *tick_data)
{
  int stdout_fd, stderr_fd;
  GError *error = NULL;
  gchar *sudo;

  // XXX - be more careful with the /tmp files by putting them in a
  //       temporary directory, maybe.

  if (!must_mkfifo ("/tmp/apt-worker.to", 0600)
      || !must_mkfifo ("/tmp/apt-worker.from", 0600)
      || !must_mkfifo ("/tmp/apt-worker.status", 0600)
      || !must_mkfifo ("/tmp/apt-worker.cancel", 0600))
    return false;

  struct stat info;
  if (stat ("/targets/links/scratchbox.config", &info))
    sudo = "/usr/bin/sudo";
  else
    sudo = "/usr/bin/fakeroot";

  gchar *options = "";

  if (break_locks)
    options = "B";

  gchar *args[] = {
    sudo,
    prog,
    "/tmp/apt-worker.to", "/tmp/apt-worker.from",
    "/tmp/apt-worker.status", "/tmp/apt-worker.cancel",
    options,
    NULL
  };

  if (!g_spawn_async_with_pipes (NULL,
				 args,
				 NULL,
				 GSpawnFlags (G_SPAWN_LEAVE_DESCRIPTORS_OPEN),
				 NULL,
				 NULL,
				 NULL,
				 NULL,
				 &stdout_fd,
				 &stderr_fd,
				 &error))
    {
      add_log ("can't spawn %s: %s\n", prog, error->message);
      g_error_free (error);
      return false;
    }

  // The order here is important and must be the same as in apt-worker
  // to avoid a dead lock.

  start_closure = new try_apt_worker_closure;
  start_closure->start_step = 0;
  start_closure->finished_cb = finished_cb;
  start_closure->finished_data = finished_data;
  start_closure->tick_cb = tick_cb;
  start_closure->tick_data = tick_data;
  start_closure->timeout_id = 0;
  start_closure->rounds_passed = 0;
  start_closure->stdout_fd = stdout_fd;
  start_closure->stderr_fd = stderr_fd;
  try_apt_worker_start (start_closure);
  
  return true;
}

void
cancel_apt_worker ()
{
  if (apt_worker_cancel_fd >= 0)
    {
      unsigned char byte = 0;
      if (write (apt_worker_cancel_fd, &byte, 1) != 1)
	log_perror ("cancel");
    }
}

static void
notice_apt_worker_failure ()
{
  //close (apt_worker_in_fd);
  //close (apt_worker_out_fd);
  //close (apt_worker_cancel_fd);

  apt_worker_in_fd = -1;
  apt_worker_out_fd = -1;
  apt_worker_cancel_fd = -1;

  cancel_all_pending_requests ();

  annoy_user_with_log (_("ai_ni_operation_failed"));
}

static bool
must_read (void *buf, size_t n)
{
  int r;

  while (n > 0)
    {
      r = read (apt_worker_in_fd, buf, n);
      if (r < 0)
	{
	  log_perror ("read");
	  return false;
	}
      else if (r == 0)
	{
	  add_log ("apt-worker exited.\n");
	  return false;
	}
      n -= r;
      buf = ((char *)buf) + r;
    }
  return true;
}

static bool
must_write (void *buf, int n)
{
  int r;

  while (n > 0)
    {
      r = write (apt_worker_out_fd, buf, n);
      if (r < 0)
	{
	  log_perror ("write");
	  return false;
	}
      else if (r == 0)
	{
	  add_log ("apt-worker exited.\n");
	  return false;
	}
      n -= r;
      buf = ((char *)buf) + r;
    }
  return true;
}

bool
apt_worker_is_running ()
{
  return apt_worker_out_fd > 0;
}

bool
send_apt_worker_request (int cmd, int state, int seq, char *data, int len)
{
  apt_request_header req = { cmd, state, seq, len };
  return must_write (&req, sizeof (req)) &&  must_write (data, len);
}

static int
next_seq ()
{
  static int seq;
  return seq++;
}

struct pending_request {
  int seq;
  apt_worker_callback *done_callback;
  void *done_data;
};

static pending_request pending[APTCMD_MAX];

void
call_apt_worker (int cmd, int state, char *data, int len,
		 apt_worker_callback *done_callback,
		 void *done_data)
{
  assert (cmd >= 0 && cmd < APTCMD_MAX);

  if (!apt_worker_started)
    {
      fprintf (stderr, "apt-worker is not running\n");
      done_callback (cmd, NULL, done_data);
    }
  else if (pending[cmd].done_callback)
    {
      fprintf (stderr, "apt-worker command %d already pending\n", cmd);
      done_callback (cmd, NULL, done_data);
    }
  else
    {
      pending[cmd].seq = next_seq ();
      pending[cmd].done_callback = done_callback;
      pending[cmd].done_data = done_data;
      if (!send_apt_worker_request (cmd, state, pending[cmd].seq, data, len))
	{
	  annoy_user_with_log (_("ai_ni_operation_failed"));
	  cancel_request (cmd);
	}
    }
}

static void
cancel_request (int cmd)
{
  apt_worker_callback *done_callback = pending[cmd].done_callback;
  void *done_data = pending[cmd].done_data;

  pending[cmd].done_callback = NULL;
  if (done_callback)
    done_callback (cmd, NULL, done_data);
}

static void
cancel_all_pending_requests ()
{
  for (int i = 0; i < APTCMD_MAX; i++)
    cancel_request (i);
}

void
handle_one_apt_worker_response ()
{
  static bool running = false;

  static apt_response_header res;
  static char *response_data = NULL;
  static int response_len = 0;
  static apt_proto_decoder dec;

  int cmd;

  assert (!running);
    
  if (!must_read (&res, sizeof (res)))
    {
      notice_apt_worker_failure ();
      return;
    }
      
  //printf ("got response %d/%d/%d\n", res.cmd, res.seq, res.len);
  cmd = res.cmd;

  if (response_len < res.len)
    {
      if (response_data)
	delete[] response_data;
      response_data = new char[res.len];
      response_len = res.len;
    }

  if (!must_read (response_data, res.len))
    {
      notice_apt_worker_failure ();
      return;
    }

  if (cmd < 0 || cmd >= APTCMD_MAX)
    {
      fprintf (stderr, "unrecognized command %d\n", res.cmd);
      return;
    }

  dec.reset (response_data, res.len);

  if (cmd == APTCMD_STATUS)
    {
      running = true;
      if (pending[cmd].done_callback)
	pending[cmd].done_callback (cmd, &dec, pending[cmd].done_data);
      running = false;
      return;
    }

  if (pending[cmd].seq != res.seq)
    {
      fprintf (stderr, "ignoring out of sequence reply.\n");
      return;
    }
  
  apt_worker_callback *done_callback = pending[cmd].done_callback;
  pending[cmd].done_callback = NULL;

  running = true;
  assert (done_callback);
  done_callback (cmd, &dec, pending[cmd].done_data);
  running = false;
}

static apt_proto_encoder request;

void
apt_worker_set_status_callback (apt_worker_callback *callback, void *data)
{
  pending[APTCMD_STATUS].done_callback = callback;
  pending[APTCMD_STATUS].done_data = data;
}

void
apt_worker_get_package_list (int state,
			     bool only_user,
			     bool only_installed,
			     bool only_available,
			     const char *pattern,
			     bool show_magic_sys,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_int (only_user);
  request.encode_int (only_installed);
  request.encode_int (only_available);
  request.encode_string (pattern);
  request.encode_int (show_magic_sys);
  call_apt_worker (APTCMD_GET_PACKAGE_LIST, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

struct awuc_closure {
  int state;
  apt_worker_callback *callback;
  void *data;
};


static void
apt_worker_update_cache_cont (bool success, void *clos)
{
  awuc_closure *c = (awuc_closure *)clos;
  apt_worker_callback *callback = c->callback;
  void *data = c->data;
  int state = c->state;
  delete c;

  if (success)
    {

      request.reset ();

      char *http_proxy = get_http_proxy ();
      request.encode_string (http_proxy);
      g_free (http_proxy);

      char *https_proxy = get_https_proxy ();
      request.encode_string (https_proxy);
      g_free (https_proxy);

      show_progress (_("ai_nw_updating_list"));
      call_apt_worker (APTCMD_UPDATE_PACKAGE_CACHE, state, 
		       request.get_buf (), request.get_len (),
		       callback, data);
    }
  else
    {
      annoy_user_with_log (_("ai_ni_update_list_not_successful"));
      callback (APTCMD_UPDATE_PACKAGE_CACHE, NULL, data);
    }
}

void
apt_worker_update_cache (int state, apt_worker_callback *callback, void *data)
{
  awuc_closure *c = new awuc_closure;
  c->callback = callback;
  c->data = data;
  c->state = state;

  ensure_network (apt_worker_update_cache_cont, c);
}

void
apt_worker_get_sources_list (apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_GET_SOURCES_LIST, APTSTATE_DEFAULT, NULL, 0,
		   callback, data);
}

void
apt_worker_set_sources_list (int state,
			     void (*encoder) (apt_proto_encoder *, void *),
			     void *encoder_data,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  encoder (&request, encoder_data);
  call_apt_worker (APTCMD_SET_SOURCES_LIST, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_catalogues (apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_GET_CATALOGUES, APTSTATE_DEFAULT, NULL, 0,
		   callback, data);
}

void
apt_worker_set_catalogues (int state, 
			   xexp *catalogues,
			   apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_xexp (catalogues);
  call_apt_worker (APTCMD_SET_CATALOGUES, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_package_info (int state,
			     const char *package,
			     bool only_installable_info,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  request.encode_int (only_installable_info);
  call_apt_worker (APTCMD_GET_PACKAGE_INFO, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_package_details (const char *package,
				const char *version,
				int summary_kind,
				apt_worker_callback *callback,
				void *data)
{
  request.reset ();
  request.encode_string (package);
  request.encode_string (version);
  request.encode_int (summary_kind);
  call_apt_worker (APTCMD_GET_PACKAGE_DETAILS, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_install_check (int state, const char *package,
			  apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_INSTALL_CHECK, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

struct awip_closure {
  char *package;
  bool updating;
  int state;
  apt_worker_callback *callback;
  void *data;
};

static void
apt_worker_install_package_cont (bool success, void *clos)
{
  awip_closure *c = (awip_closure *)clos;
  char *package = c->package;
  apt_worker_callback *callback = c->callback;
  void *data = c->data;
  bool updating = c->updating;
  int state = c->state;
  delete c;

  if (success)
    {
      request.reset ();
      request.encode_string (package);

      char *http_proxy = get_http_proxy ();
      request.encode_string (http_proxy);
      g_free (http_proxy);

      char *https_proxy = get_https_proxy ();
      request.encode_string (https_proxy);
      g_free (https_proxy);

      set_general_progress_title (updating 
				  ? _("ai_nw_updating")
				  : _("ai_nw_installing"));
      reset_progress_was_cancelled ();

      call_apt_worker (APTCMD_INSTALL_PACKAGE, state,
		       request.get_buf (), request.get_len (),
		       callback, data);
    }
  else
    callback (APTCMD_INSTALL_PACKAGE, NULL, data);

  g_free (package);
}

void
apt_worker_install_package (int state, const char *package, bool updating,
			    apt_worker_callback *callback, void *data)
{
  awip_closure *c = new awip_closure;
  c->package = g_strdup (package);
  c->callback = callback;
  c->data = data;
  c->updating = updating;
  c->state = state;

  ensure_network (apt_worker_install_package_cont, c);
}

void
apt_worker_get_packages_to_remove (const char *package,
				   apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_GET_PACKAGES_TO_REMOVE, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_remove_package (const char *package,
			   apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_REMOVE_PACKAGE, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_clean (int state, apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_CLEAN, state, NULL, 0,
		   callback, data);
}

void
apt_worker_install_file (const char *file,
			 apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (file);
  call_apt_worker (APTCMD_INSTALL_FILE, APTSTATE_DEFAULT, 
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_file_details (bool only_user, const char *file,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_int (only_user);
  request.encode_string (file);
  call_apt_worker (APTCMD_GET_FILE_DETAILS, APTSTATE_DEFAULT, 
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_save_applications_install_file (apt_worker_callback *callback,
					   void *data)
{
  request.reset ();
  call_apt_worker (APTCMD_SAVE_APPLICATIONS_INSTALL_FILE, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

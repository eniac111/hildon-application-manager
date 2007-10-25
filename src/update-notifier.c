/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007 Nokia Corporation.  All Rights reserved.
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

/* XXX

   - Localize
   - Make sure icon doesn't blink when screen is off
   - Plug all the leaks
*/

#include <glib.h>
#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <libhildondesktop/libhildondesktop.h>
#include <libhildondesktop/statusbar-item.h>

#include <gconf/gconf-client.h>
#include <dbus/dbus.h>

#include "update-notifier.h"
#include "pixbufblinkifier.h"
#include "xexp.h"

#define USE_BLINKIFIER 1

typedef struct _UpdateNotifier      UpdateNotifier;
typedef struct _UpdateNotifierClass UpdateNotifierClass;

#define UPDATE_NOTIFIER_TYPE            (update_notifier_get_type ())
#define UPDATE_NOTIFIER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), UPDATE_NOTIFIER_TYPE, UpdateNotifier))
#define UPDATE_NOTIFIER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  UPDATE_NOTIFIER_TYPE, UpdateNotifierClass))
#define IS_UPDATE_NOTIFIER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), UPDATE_NOTIFIER_TYPE))
#define IS_UPDATE_NOTIFIER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  UPDATE_NOTIFIER_TYPE))
#define UPDATE_NOTIFIER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  UPDATE_NOTIFIER_TYPE, UpdateNotifierClass))

struct _UpdateNotifier
{
  StatusbarItem parent;

  GtkWidget *button;
  GtkWidget *blinkifier;
  GtkWidget *menu;

  guint timeout_id;

  GConfClient *gconf;
  DBusConnection *dbus;
  
  gboolean checking_active;
};

struct _UpdateNotifierClass
{
  StatusbarItemClass parent_class;
};

GType update_notifier_get_type(void);

HD_DEFINE_PLUGIN (UpdateNotifier, update_notifier, STATUSBAR_TYPE_ITEM);

static void set_icon_visibility (UpdateNotifier *upno, int state);

static void setup_dbus (UpdateNotifier *upno);
static void setup_http_proxy ();

static void update_icon_visibility (UpdateNotifier *upno, GConfValue *value);
static void update_menu (UpdateNotifier *upno);

static void show_check_for_updates_view (UpdateNotifier *upno);
static void check_for_updates (UpdateNotifier *upno);

static void update_notifier_finalize (GObject *object);

static void
update_notifier_class_init (UpdateNotifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = update_notifier_finalize;
}


static void
menu_position_func (GtkMenu   *menu, 
		    gint      *x, 
		    gint      *y,
		    gboolean  *push_in, 
		    gpointer   data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);
  
  GtkRequisition req;
  
  gtk_widget_size_request (GTK_WIDGET (menu->toplevel), &req);

  gdk_window_get_origin (upno->button->window, x, y);
  *x += (upno->button->allocation.x
	 + upno->button->allocation.width
	 - req.width);
  *y += (upno->button->allocation.y
	 + upno->button->allocation.height);

  *push_in = FALSE;
}

static void
button_pressed (GtkWidget *button, gpointer data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);

  set_icon_visibility (upno, UPNO_ICON_STATIC);

  update_menu (upno);

  gtk_menu_popup (GTK_MENU (upno->menu),
		  NULL, NULL,
		  menu_position_func, upno,
		  1,
		  gtk_get_current_event_time ());
}

static void
menu_activated (GtkWidget *menu, gpointer data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);

  show_check_for_updates_view (upno);
}

static void
gconf_state_changed (GConfClient *client, guint cnxn_id,
		     GConfEntry *entry, gpointer data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);

  update_icon_visibility (upno, entry->value);
}

static void
update_notifier_init (UpdateNotifier *upno)
{
  GtkWidget *item;
  GdkPixbuf *icon_pixbuf;
  GtkIconTheme *icon_theme;

  upno->gconf = gconf_client_get_default();
  gconf_client_add_dir (upno->gconf,
			UPNO_GCONF_DIR,
			GCONF_CLIENT_PRELOAD_ONELEVEL,
			NULL);
  gconf_client_notify_add (upno->gconf,
			   UPNO_GCONF_STATE,
			   gconf_state_changed, upno,
			   NULL, NULL);

  upno->button = gtk_button_new ();

  icon_theme = gtk_icon_theme_get_default ();
  icon_pixbuf = gtk_icon_theme_load_icon (icon_theme,
					  "qgn_stat_new_updates",
					  40,
					  GTK_ICON_LOOKUP_NO_SVG,
					  NULL);
#if USE_BLINKIFIER
  upno->blinkifier = g_object_new (PIXBUF_BLINKIFIER_TYPE,
				   "pixbuf", icon_pixbuf,
				   "frame-time", 100,
				   "n-frames", 10,
				   NULL);
#else
  upno->blinkifier = gtk_image_new_from_pixbuf (icon_pixbuf);
#endif
  
  gtk_container_add (GTK_CONTAINER (upno->button), upno->blinkifier);
  gtk_container_add (GTK_CONTAINER (upno), upno->button);

  gtk_widget_show (upno->blinkifier);
  gtk_widget_show (upno->button);

  upno->menu = gtk_menu_new ();
  item = gtk_menu_item_new_with_label ("Foo");
  gtk_menu_append (upno->menu, item);
  gtk_widget_show (item);

  g_signal_connect (upno->button, "pressed",
		    G_CALLBACK (button_pressed), upno);

  setup_dbus (upno);

  update_menu (upno);
  update_icon_visibility (upno, gconf_client_get (upno->gconf,
						  UPNO_GCONF_STATE,
						  NULL));
}

static void
update_notifier_finalize (GObject *object)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (object);
    
  G_OBJECT_CLASS (g_type_class_peek_parent
		  (G_OBJECT_GET_CLASS(object)))->finalize(object);
}

#if !USE_BLINKIFIER
static gboolean
blink_icon (gpointer data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);
  
  if (GTK_WIDGET_VISIBLE (upno->blinkifier))
    gtk_widget_hide (upno->blinkifier);
  else
    gtk_widget_show (upno->blinkifier);

  return TRUE;
}
#endif

static void
update_icon_visibility (UpdateNotifier *upno, GConfValue *value)
{
  int state = UPNO_ICON_INVISIBLE;

  if (value && value->type == GCONF_VALUE_INT)
    state = gconf_value_get_int (value);

  g_object_set (upno,
		"condition", (state == UPNO_ICON_STATIC
			      || state == UPNO_ICON_BLINKING),
		NULL);

#if USE_BLINKIFIER
  g_object_set (upno->blinkifier,
		"blinking", (state == UPNO_ICON_BLINKING),
		NULL);
#else
  if (state == UPNO_ICON_BLINKING)
    {
      if (upno->timeout_id == 0)
	upno->timeout_id = g_timeout_add (500, blink_icon, upno);
    }
  else
    {
      gtk_widget_show (upno->blinkifier);
      if (upno->timeout_id > 0)
	{
	  g_source_remove (upno->timeout_id);
	  upno->timeout_id = 0;
	}
    }
#endif
}

static void
add_readonly_item (GtkWidget *menu, const char *fmt, ...)
{
  GtkWidget *item;
  va_list ap;
  char *label;

  va_start (ap, fmt);
  label = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  item = gtk_menu_item_new_with_label (label);
  gtk_menu_append (GTK_MENU (menu), item);
  gtk_widget_show (item);
  gtk_widget_set_sensitive (item, FALSE);

  g_free (label);
}

static void
update_menu (UpdateNotifier *upno)
{
  xexp *updates;
  int n_os = 0, n_nokia = 0, n_other = 0;
  GtkWidget *item;

  /* XXX - only do this when file has actually changed.
   */

  updates = xexp_read_file ("/var/lib/hildon-application-manager/available-updates");

  if (updates)
    {
      xexp *x;

      if ((x = xexp_aref (updates, "os-updates")))
	n_os = xexp_aref_int (x, "count", 0);
      
      if ((x = xexp_aref (updates, "nokia-updates")))
	n_nokia = xexp_aref_int (x, "count", 0);

      if ((x = xexp_aref (updates, "other-updates")))
	n_other = xexp_aref_int (x, "count", 0);

      xexp_free (updates);
    }

  if (upno->menu)
    gtk_widget_destroy (upno->menu);

  upno->menu = gtk_menu_new ();

  add_readonly_item (upno->menu, "Available software updates:");
  add_readonly_item (upno->menu, "   Nokia (%d)", n_nokia);
  add_readonly_item (upno->menu, "   Other (%d)", n_other);
  add_readonly_item (upno->menu, "   OS (%d)", n_os);

  item = gtk_separator_menu_item_new ();
  gtk_menu_append (upno->menu, item);
  gtk_widget_show (item);
  
  item = gtk_menu_item_new_with_label ("Invoke Application Manager");
  gtk_menu_append (upno->menu, item);
  gtk_widget_show (item);
  g_signal_connect (item, "activate",
		    G_CALLBACK (menu_activated), upno);
}

static void
set_icon_visibility (UpdateNotifier *upno, int state)
{
  gconf_client_set_int (upno->gconf, 
			UPNO_GCONF_STATE,
			state,
			NULL);
}

static DBusHandlerResult 
dbus_filter (DBusConnection *conn, DBusMessage *message, void *data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_update_notifier",
				   "check_for_updates"))
    {
      DBusMessage *reply;

      check_for_updates (upno);

      reply = dbus_message_new_method_return (message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
setup_dbus (UpdateNotifier *upno)
{
  upno->dbus = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (upno->dbus)
    {
      dbus_connection_add_filter (upno->dbus, dbus_filter, upno, NULL);
      dbus_bus_request_name (upno->dbus,
			     "com.nokia.hildon_update_notifier",
			     DBUS_NAME_FLAG_DO_NOT_QUEUE,
			     NULL);
    }
}

static void
show_check_for_updates_view (UpdateNotifier *upno)
{
  DBusMessage     *msg;
  gchar           *service = "com.nokia.hildon_application_manager";
  gchar           *object_path = "/com/nokia/hildon_application_manager";
  gchar           *interface = "com.nokia.hildon_application_manager";

  if (upno->dbus)
    {
      msg = dbus_message_new_method_call (service, object_path,
					  interface,
					  "show_check_for_updates_view");
      if (msg)
	{
	  dbus_connection_send (upno->dbus, msg, NULL);
	  dbus_message_unref (msg);
	}
    }
}

static void
check_for_updates_done (GPid pid, int status, gpointer data)
{
  UpdateNotifier *upno = UPDATE_NOTIFIER (data);

  upno->checking_active = FALSE;

  if (status != -1 && WIFEXITED (status) && WEXITSTATUS (status) == 0)
    {
      /* XXX - only blink if there is something new, of course...
       */
      set_icon_visibility (upno, UPNO_ICON_BLINKING);
    }
  else
    {
      /* Ask the Application Manager to do perform the update, but
	 don't start it if it isn't running already.
       */

      DBusMessage     *msg;
      gchar           *service = "com.nokia.hildon_application_manager";
      gchar           *object_path = "/com/nokia/hildon_application_manager";
      gchar           *interface = "com.nokia.hildon_application_manager";
      
      fprintf (stderr, "FAILED: %d\n", status);

      if (upno->dbus)
	{
	  msg = dbus_message_new_method_call (service, object_path,
					      interface,
					      "check_for_updates");
	  if (msg)
	    {
	      dbus_message_set_auto_start (msg, FALSE);
	      dbus_connection_send (upno->dbus, msg, NULL);
	      dbus_message_unref (msg);
	    }
	}
    }
}

static void
check_for_updates (UpdateNotifier *upno)
{
  GError *error = NULL;
  GPid child_pid;
  char *argv[] = { "/usr/libexec/apt-worker", "--check-updates", NULL };

  if (upno->checking_active)
    return;

  upno->checking_active = TRUE;

  setup_http_proxy ();

  if (!g_spawn_async_with_pipes (NULL,
				 argv,
				 NULL,
				 G_SPAWN_DO_NOT_REAP_CHILD,
				 NULL,
				 NULL,
				 &child_pid,
				 NULL,
				 NULL,
				 NULL,
				 &error))
    {
      fprintf (stderr, "can't run %s: %s\n", argv[0], error->message);
      g_error_free (error);
      return;
    }

  g_child_watch_add (child_pid, check_for_updates_done, upno);
}

static void
setup_http_proxy ()
{
  GConfClient *conf;
  char *proxy;

  if ((proxy = getenv ("http_proxy")) != NULL)
    return;

  conf = gconf_client_get_default ();

  if (gconf_client_get_bool (conf, "/system/http_proxy/use_http_proxy",
			     NULL))
    {
      char *user = NULL;
      char *password = NULL;
      char *host = NULL;
      gint port;

      if (gconf_client_get_bool (conf, "/system/http_proxy/use_authentication",
				 NULL))
	{
	  user = gconf_client_get_string
	    (conf, "/system/http_proxy/authentication_user", NULL);
	  password = gconf_client_get_string
	    (conf, "/system/http_proxy/authentication_password", NULL);
	}

      host = gconf_client_get_string (conf, "/system/http_proxy/host", NULL);
      port = gconf_client_get_int (conf, "/system/http_proxy/port", NULL);

      if (user)
	{
	  // XXX - encoding of '@', ':' in user and password?

	  if (password)
	    proxy = g_strdup_printf ("http://%s:%s@%s:%d",
				     user, password, host, port);
	  else
	    proxy = g_strdup_printf ("http://%s@%s:%d", user, host, port);
	}
      else
	proxy = g_strdup_printf ("http://%s:%d", host, port);

      g_free (user);
      g_free (password);
      g_free (host);

      /* XXX - there is also ignore_hosts, which we ignore for now,
	       since transcribing it to no_proxy is hard... mandatory,
	       non-transparent proxies are evil anyway.
      */

      setenv ("http_proxy", proxy, 1);
      g_free (proxy);
    }

  g_object_unref (conf);
}
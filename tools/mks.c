/* mks.c
 *
 * Copyright 2026 Christian Hergert <christian@sourceandstack.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <unistd.h>

#include <gtk/gtk.h>
#include <libmks.h>

static GDBusConnection *
create_connection (int      argc,
                   char   **argv,
                   GError **error)
{
  if (argc < 2)
    return g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  else
    return g_dbus_connection_new_for_address_sync (argv[1], G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, error);
}

static gboolean
update_title_binding (GBinding     *binding,
                      const GValue *from_value,
                      GValue       *to_value,
                      gpointer      user_data)
{
  MksDisplay *display = user_data;
  GtkShortcutTrigger *trigger = mks_display_get_ungrab_trigger (display);
  g_autofree char *label = gtk_shortcut_trigger_to_label (trigger, gtk_widget_get_display (GTK_WIDGET (display)));

  if (g_value_get_boolean (from_value))
    g_value_take_string (to_value, g_strdup_printf ("MKS (%s to ungrab)", label));
  else
    g_value_set_static_string (to_value, "MKS");

  return TRUE;
}

typedef struct
{
  int         argc;
  char      **argv;
  GMainLoop  *main_loop;
} Main;

static DexFuture *
main_fiber (gpointer user_data)
{
  Main *state = user_data;
  g_autoptr(MksTransport) transport = NULL;
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(MksSession) session = NULL;
  g_autoptr(MksScreen) screen = NULL;
  g_autoptr(MksClipboard) clipboard = NULL;
  g_autoptr(MksClipboardRedirector) clipboard_redirector = NULL;
  g_autoptr(GError) error = NULL;
  GtkWindow *window;
  GtkWidget *display;
  GtkSettings *settings;
  GdkSurface *surface;

  g_assert (state != NULL);
  g_assert (state->main_loop != NULL);

  if (!(connection = create_connection (state->argc, state->argv, &error)))
    {
      g_printerr ("Failed to connect to D-Bus: %s\n", error->message);
      return dex_future_new_for_int (EXIT_FAILURE);
    }

  window = g_object_new (GTK_TYPE_WINDOW,
                         "default-width", 1280,
                         "default-height", 768,
                         "title", "Mouse, Keyboard, Screen",
                         NULL);

  settings = gtk_settings_get_default ();
  g_object_set (settings,
                "gtk-application-prefer-dark-theme", TRUE,
                NULL);

  display = mks_display_new ();
  gtk_window_set_child (window, display);
  g_signal_connect_swapped (window,
                            "close-request",
                            G_CALLBACK (g_main_loop_quit),
                            state->main_loop);

  transport = mks_dbus_transport_new (connection, "org.qemu");

  if (!(session = dex_await_object (mks_session_new (transport), &error)))
    {
      g_printerr ("Failed to create MksSession: %s\n", error->message);
      return dex_future_new_for_int (EXIT_FAILURE);
    }

  if (!(screen = mks_session_dup_primary_screen (session)))
    {
      g_printerr ("No screen attached to session!\n");
      return dex_future_new_for_int (EXIT_FAILURE);
    }

  g_object_bind_property (session, "primary-screen",
                          display, "screen",
                          (G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE));

  if ((clipboard = mks_session_dup_clipboard (session)))
    {
      clipboard_redirector = mks_clipboard_redirector_new (clipboard,
                                                            gtk_widget_get_display (display));

      /* main_fiber() returns after setup, so keep the redirector alive for
       * exactly as long as the viewer window instead of dropping it here. */
      g_object_set_data_full (G_OBJECT (window),
                              "mks-clipboard-redirector",
                              g_steal_pointer (&clipboard_redirector),
                              g_object_unref);
    }
  else
    g_printerr ("No clipboard attached to session!\n");

  gtk_window_present (window);

  surface = gtk_native_get_surface (GTK_NATIVE (window));
  g_object_bind_property_full (surface, "shortcuts-inhibited",
                               window, "title",
                               G_BINDING_SYNC_CREATE,
                               update_title_binding, NULL, display, NULL);

  return dex_future_new_for_int (EXIT_SUCCESS);
}

static DexFuture *
main_loop_quit_cb (DexFuture *future,
                   gpointer   user_data)
{
  g_main_loop_quit (user_data);

  return dex_ref (future);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(GMainLoop) setup_loop = NULL;
  g_autoptr(GMainLoop) main_loop = NULL;
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GError) error = NULL;
  const GValue *value;
  Main state;
  int ret;

  dex_init ();
  gtk_init ();
  mks_init ();

  setup_loop = g_main_loop_new (NULL, FALSE);
  main_loop = g_main_loop_new (NULL, FALSE);
  state = (Main) { argc, argv, main_loop };

  future = dex_future_finally (dex_scheduler_spawn (NULL,
                                                    8 * 1024 * 1024,
                                                    main_fiber,
                                                    &state,
                                                    NULL),
                               main_loop_quit_cb,
                               g_main_loop_ref (setup_loop),
                               (GDestroyNotify) g_main_loop_unref);

  if (dex_future_is_pending (future))
    g_main_loop_run (setup_loop);

  if (!(value = dex_future_get_value (future, &error)))
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }

  ret = g_value_get_int (value);
  if (ret != EXIT_SUCCESS)
    return ret;

  g_main_loop_run (main_loop);

  return ret;
}

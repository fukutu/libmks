/* mks-clipboard-redirector.c
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

#include "mks-clipboard-private.h"
#include "mks-clipboard-redirector.h"
#include "mks-enums.h"

typedef struct
{
  MksClipboardRedirector *redirector;
  MksClipboardSelection   selection;
  DexPromise             *promise;
  GInputStream           *input;
  GOutputStream          *stream;
  char                   *mime_type;
} ReadState;

static void mks_remote_content_provider_set_property (GObject      *object,
                                                      guint         prop_id,
                                                      const GValue *value,
                                                      GParamSpec   *pspec);

#define MKS_TYPE_REMOTE_CONTENT_PROVIDER (mks_remote_content_provider_get_type())

G_DECLARE_FINAL_TYPE (MksRemoteContentProvider, mks_remote_content_provider, MKS, REMOTE_CONTENT_PROVIDER, GdkContentProvider)

struct _MksRemoteContentProvider
{
  GdkContentProvider parent_instance;

  MksClipboard           *clipboard;
  MksClipboardSelection   selection;
  char                  **mime_types;
};

struct _MksClipboardRedirector
{
  GObject parent_instance;

  MksClipboard                     *clipboard;
  GdkDisplay                       *display;
  GdkClipboard                     *gdk_clipboard;
  GdkClipboard                     *gdk_primary;
  MksClipboardRedirectorSelections  selections;
  gboolean                          enabled;
  gboolean                          applying_remote;

  gulong clipboard_changed_handler;
  gulong primary_changed_handler;
  gulong mks_changed_handler;
};

G_DEFINE_FINAL_TYPE (MksRemoteContentProvider, mks_remote_content_provider, GDK_TYPE_CONTENT_PROVIDER)
G_DEFINE_FINAL_TYPE (MksClipboardRedirector, mks_clipboard_redirector, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_CLIPBOARD,
  PROP_DISPLAY,
  PROP_ENABLED,
  PROP_SELECTIONS,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
read_state_free (ReadState *state)
{
  g_clear_object (&state->redirector);
  dex_clear (&state->promise);
  g_clear_object (&state->input);
  g_clear_object (&state->stream);
  g_clear_pointer (&state->mime_type, g_free);
  g_free (state);
}

static void
mks_clipboard_redirector_splice_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (object);
  ReadState *state = user_data;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(MksClipboardContent) content = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_OUTPUT_STREAM (stream));
  g_assert (state != NULL);

  if (g_output_stream_splice_finish (stream, result, &error) < 0)
    {
      dex_promise_reject (state->promise, g_steal_pointer (&error));
      read_state_free (state);
      return;
    }

  bytes = g_bytes_new (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (state->stream)),
                       g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (state->stream)));
  content = mks_clipboard_content_new (state->mime_type, bytes);

  dex_promise_resolve_boxed (state->promise,
                             MKS_TYPE_CLIPBOARD_CONTENT,
                             g_steal_pointer (&content));

  read_state_free (state);
}

static void
mks_clipboard_redirector_read_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GdkClipboard *clipboard = GDK_CLIPBOARD (object);
  ReadState *state = user_data;
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GError) error = NULL;
  const char *mime_type = NULL;

  g_assert (GDK_IS_CLIPBOARD (clipboard));
  g_assert (state != NULL);

  if (!(input = gdk_clipboard_read_finish (clipboard,
                                           result,
                                           &mime_type,
                                           &error)))
    {
      dex_promise_reject (state->promise, g_steal_pointer (&error));
      read_state_free (state);
      return;
    }

  if (mime_type == NULL)
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_NOT_SUPPORTED,
                                   "Clipboard content type is not available");
      dex_promise_reject (state->promise, g_steal_pointer (&error));
      read_state_free (state);
      return;
    }

  state->input = g_steal_pointer (&input);
  state->mime_type = g_strdup (mime_type);

  g_output_stream_splice_async (state->stream,
                                state->input,
                                G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                G_PRIORITY_DEFAULT,
                                dex_promise_get_cancellable (state->promise),
                                mks_clipboard_redirector_splice_cb,
                                state);
}

static MksClipboardSelection
selection_from_clipboard (MksClipboardRedirector *self,
                          GdkClipboard           *clipboard)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (GDK_IS_CLIPBOARD (clipboard));

  if (clipboard == self->gdk_primary)
    return MKS_CLIPBOARD_SELECTION_PRIMARY;

  return MKS_CLIPBOARD_SELECTION_CLIPBOARD;
}

static GdkClipboard *
clipboard_from_selection (MksClipboardRedirector *self,
                          MksClipboardSelection   selection)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));

  if (selection == MKS_CLIPBOARD_SELECTION_PRIMARY)
    return self->gdk_primary;

  return self->gdk_clipboard;
}

static gboolean
selection_is_enabled (MksClipboardRedirector *self,
                      MksClipboardSelection   selection)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));

  if (selection == MKS_CLIPBOARD_SELECTION_PRIMARY)
    return !!(self->selections & MKS_CLIPBOARD_REDIRECTOR_SELECTION_PRIMARY);

  if (selection == MKS_CLIPBOARD_SELECTION_CLIPBOARD)
    return !!(self->selections & MKS_CLIPBOARD_REDIRECTOR_SELECTION_CLIPBOARD);

  return FALSE;
}

static void
mks_clipboard_redirector_update_registration (MksClipboardRedirector *self)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));

  if (self->clipboard == NULL)
    return;

  if (self->enabled)
    dex_future_disown (mks_clipboard_register (self->clipboard));
  else if (mks_clipboard_get_registered (self->clipboard))
    dex_future_disown (mks_clipboard_unregister (self->clipboard));
}

static void
mks_clipboard_redirector_claim (MksClipboardRedirector *self,
                                GdkClipboard           *clipboard)
{
  g_autoptr(GdkContentFormats) formats = NULL;
  const char * const *mime_types;
  gsize n_mime_types;
  MksClipboardSelection selection;

  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (GDK_IS_CLIPBOARD (clipboard));

  if (!self->enabled || self->applying_remote)
    return;

  selection = selection_from_clipboard (self, clipboard);

  if (!selection_is_enabled (self, selection))
    return;

  if (MKS_IS_REMOTE_CONTENT_PROVIDER (gdk_clipboard_get_content (clipboard)))
    return;

  formats = gdk_content_formats_ref (gdk_clipboard_get_formats (clipboard));
  mime_types = gdk_content_formats_get_mime_types (formats, &n_mime_types);

  if (n_mime_types == 0)
    return;

  dex_future_disown (mks_clipboard_claim (self->clipboard, selection, mime_types));
}

static void
mks_clipboard_redirector_clipboard_changed_cb (MksClipboardRedirector *self,
                                               GdkClipboard           *clipboard)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (GDK_IS_CLIPBOARD (clipboard));

  mks_clipboard_redirector_claim (self, clipboard);
}

static void
mks_clipboard_redirector_remote_changed_cb (MksClipboardRedirector *self,
                                            MksClipboardSelection   selection,
                                            MksClipboard           *clipboard)
{
  g_autoptr(GdkContentProvider) provider = NULL;
  g_auto(GStrv) mime_types = NULL;
  GdkClipboard *gdk_clipboard;

  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (MKS_IS_CLIPBOARD (clipboard));

  if (!self->enabled ||
      !selection_is_enabled (self, selection) ||
      mks_clipboard_get_owner (clipboard, selection) != MKS_CLIPBOARD_OWNER_REMOTE)
    return;

  if (!(mime_types = mks_clipboard_dup_mime_types (clipboard, selection)))
    return;

  gdk_clipboard = clipboard_from_selection (self, selection);
  provider = g_object_new (MKS_TYPE_REMOTE_CONTENT_PROVIDER,
                           "clipboard", clipboard,
                           "selection", selection,
                           "mime-types", mime_types,
                           NULL);

  self->applying_remote = TRUE;
  gdk_clipboard_set_content (gdk_clipboard, provider);
  self->applying_remote = FALSE;
}

static DexFuture *
mks_clipboard_redirector_read (MksClipboard          *clipboard,
                               MksClipboardSelection  selection,
                               const char * const    *mime_types,
                               gpointer               user_data)
{
  MksClipboardRedirector *self = user_data;
  GdkClipboard *gdk_clipboard;
  ReadState *state;

  g_assert (MKS_IS_CLIPBOARD (clipboard));
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (mime_types != NULL);

  if (!self->enabled || !selection_is_enabled (self, selection))
    return dex_future_new_reject (G_IO_ERROR,
                                  G_IO_ERROR_NOT_FOUND,
                                  "Clipboard redirection is disabled");

  gdk_clipboard = clipboard_from_selection (self, selection);

  state = g_new0 (ReadState, 1);
  state->redirector = g_object_ref (self);
  state->selection = selection;
  state->promise = dex_promise_new_cancellable ();
  state->stream = g_memory_output_stream_new_resizable ();

  gdk_clipboard_read_async (gdk_clipboard,
                            (const char **) mime_types,
                            G_PRIORITY_DEFAULT,
                            dex_promise_get_cancellable (state->promise),
                            mks_clipboard_redirector_read_cb,
                            state);

  return DEX_FUTURE (dex_ref (state->promise));
}

static void
mks_clipboard_redirector_set_clipboard (MksClipboardRedirector *self,
                                        MksClipboard           *clipboard)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (!clipboard || MKS_IS_CLIPBOARD (clipboard));

  if (self->clipboard != NULL)
    return;

  if (g_set_object (&self->clipboard, clipboard))
    {
      self->mks_changed_handler =
        g_signal_connect_object (clipboard,
                                 "changed",
                                 G_CALLBACK (mks_clipboard_redirector_remote_changed_cb),
                                 self,
                                 G_CONNECT_SWAPPED);

      _mks_clipboard_set_read_func (clipboard,
                                    mks_clipboard_redirector_read,
                                    g_object_ref (self),
                                    g_object_unref);
      mks_clipboard_redirector_update_registration (self);
    }
}

static void
mks_clipboard_redirector_set_display (MksClipboardRedirector *self,
                                      GdkDisplay             *display)
{
  g_assert (MKS_IS_CLIPBOARD_REDIRECTOR (self));
  g_assert (!display || GDK_IS_DISPLAY (display));

  if (self->display != NULL)
    return;

  if (g_set_object (&self->display, display))
    {
      self->gdk_clipboard = gdk_display_get_clipboard (display);
      self->gdk_primary = gdk_display_get_primary_clipboard (display);

      self->clipboard_changed_handler =
        g_signal_connect_object (self->gdk_clipboard,
                                 "changed",
                                 G_CALLBACK (mks_clipboard_redirector_clipboard_changed_cb),
                                 self,
                                 G_CONNECT_SWAPPED);

      self->primary_changed_handler =
        g_signal_connect_object (self->gdk_primary,
                                 "changed",
                                 G_CALLBACK (mks_clipboard_redirector_clipboard_changed_cb),
                                 self,
                                 G_CONNECT_SWAPPED);
    }
}

static void
mks_clipboard_redirector_dispose (GObject *object)
{
  MksClipboardRedirector *self = (MksClipboardRedirector *)object;

  if (self->clipboard != NULL)
    {
      if (mks_clipboard_get_registered (self->clipboard))
        dex_future_disown (mks_clipboard_unregister (self->clipboard));

      _mks_clipboard_set_read_func (self->clipboard, NULL, NULL, NULL);
      g_clear_signal_handler (&self->mks_changed_handler, self->clipboard);
      g_clear_object (&self->clipboard);
    }

  if (self->gdk_clipboard != NULL)
    g_clear_signal_handler (&self->clipboard_changed_handler, self->gdk_clipboard);

  if (self->gdk_primary != NULL)
    g_clear_signal_handler (&self->primary_changed_handler, self->gdk_primary);

  self->gdk_clipboard = NULL;
  self->gdk_primary = NULL;
  g_clear_object (&self->display);

  G_OBJECT_CLASS (mks_clipboard_redirector_parent_class)->dispose (object);
}

static void
mks_clipboard_redirector_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  MksClipboardRedirector *self = MKS_CLIPBOARD_REDIRECTOR (object);

  switch (prop_id)
    {
    case PROP_CLIPBOARD:
      g_value_set_object (value, mks_clipboard_redirector_get_clipboard (self));
      break;

    case PROP_DISPLAY:
      g_value_set_object (value, mks_clipboard_redirector_get_display (self));
      break;

    case PROP_ENABLED:
      g_value_set_boolean (value, mks_clipboard_redirector_get_enabled (self));
      break;

    case PROP_SELECTIONS:
      g_value_set_flags (value, mks_clipboard_redirector_get_selections (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_clipboard_redirector_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  MksClipboardRedirector *self = MKS_CLIPBOARD_REDIRECTOR (object);

  switch (prop_id)
    {
    case PROP_CLIPBOARD:
      mks_clipboard_redirector_set_clipboard (self, g_value_get_object (value));
      break;

    case PROP_DISPLAY:
      mks_clipboard_redirector_set_display (self, g_value_get_object (value));
      break;

    case PROP_ENABLED:
      mks_clipboard_redirector_set_enabled (self, g_value_get_boolean (value));
      break;

    case PROP_SELECTIONS:
      mks_clipboard_redirector_set_selections (self, g_value_get_flags (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_clipboard_redirector_class_init (MksClipboardRedirectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = mks_clipboard_redirector_dispose;
  object_class->get_property = mks_clipboard_redirector_get_property;
  object_class->set_property = mks_clipboard_redirector_set_property;

  properties[PROP_CLIPBOARD] =
    g_param_spec_object ("clipboard", NULL, NULL,
                         MKS_TYPE_CLIPBOARD,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  properties[PROP_DISPLAY] =
    g_param_spec_object ("display", NULL, NULL,
                         GDK_TYPE_DISPLAY,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  properties[PROP_ENABLED] =
    g_param_spec_boolean ("enabled", NULL, NULL,
                          TRUE,
                          (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  properties[PROP_SELECTIONS] =
    g_param_spec_flags ("selections", NULL, NULL,
                        MKS_TYPE_CLIPBOARD_REDIRECTOR_SELECTIONS, MKS_CLIPBOARD_REDIRECTOR_SELECTION_CLIPBOARD,
                        (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
mks_clipboard_redirector_init (MksClipboardRedirector *self)
{
  self->enabled = TRUE;
  self->selections = MKS_CLIPBOARD_REDIRECTOR_SELECTION_CLIPBOARD;
}

static GdkContentFormats *
mks_remote_content_provider_ref_formats (GdkContentProvider *provider)
{
  MksRemoteContentProvider *self = MKS_REMOTE_CONTENT_PROVIDER (provider);

  return gdk_content_formats_new ((const char **) self->mime_types,
                                  g_strv_length (self->mime_types));
}

typedef struct
{
  GOutputStream *stream;
  GBytes        *bytes;
} RemoteWriteState;

static void
remote_write_state_free (RemoteWriteState *state)
{
  g_clear_object (&state->stream);
  g_clear_pointer (&state->bytes, g_bytes_unref);
  g_free (state);
}

static void
mks_remote_content_provider_write_cb (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (object);
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  gsize bytes_written;

  g_assert (G_IS_OUTPUT_STREAM (stream));
  g_assert (G_IS_TASK (task));

  if (!g_output_stream_write_all_finish (stream, result, &bytes_written, &error))
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_boolean (task, TRUE);
}

static void
mks_remote_content_provider_request_cb (GObject      *object,
                                        GAsyncResult *result,
                                        gpointer      user_data)
{
  MksClipboard *clipboard = MKS_CLIPBOARD (object);
  g_autoptr(GTask) task = user_data;
  RemoteWriteState *state;
  g_autoptr(MksClipboardContent) content = NULL;
  g_autoptr(GError) error = NULL;
  GOutputStream *stream;
  gconstpointer data;
  gsize size;

  g_assert (MKS_IS_CLIPBOARD (clipboard));
  g_assert (G_IS_TASK (task));

  if (!(content = mks_clipboard_request_finish (clipboard, result, &error)))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  state = g_task_get_task_data (task);
  g_assert (state != NULL);

  stream = state->stream;
  state->bytes = mks_clipboard_content_dup_bytes (content);
  data = g_bytes_get_data (state->bytes, &size);

  /* write_all_async() does not copy @data; keep the GBytes alive in @task. */
  g_output_stream_write_all_async (stream,
                                   data,
                                   size,
                                   G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (task),
                                   mks_remote_content_provider_write_cb,
                                   g_object_ref (task));
}

static void
mks_remote_content_provider_write_mime_type_async (GdkContentProvider  *provider,
                                                   const char          *mime_type,
                                                   GOutputStream       *stream,
                                                   int                  io_priority,
                                                   GCancellable        *cancellable,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data)
{
  MksRemoteContentProvider *self = MKS_REMOTE_CONTENT_PROVIDER (provider);
  RemoteWriteState *state;
  g_autoptr(GTask) task = NULL;
  const char *mime_types[] = { NULL, NULL };

  g_assert (MKS_IS_REMOTE_CONTENT_PROVIDER (self));
  g_assert (mime_type != NULL);
  g_assert (G_IS_OUTPUT_STREAM (stream));

  task = g_task_new (provider, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);

  state = g_new0 (RemoteWriteState, 1);
  state->stream = g_object_ref (stream);
  g_task_set_task_data (task, state, (GDestroyNotify) remote_write_state_free);

  mime_types[0] = mime_type;

  mks_clipboard_request_async (self->clipboard,
                               self->selection,
                               mime_types,
                               cancellable,
                               mks_remote_content_provider_request_cb,
                               g_object_ref (task));
}

static gboolean
mks_remote_content_provider_write_mime_type_finish (GdkContentProvider  *provider,
                                                    GAsyncResult        *result,
                                                    GError             **error)
{
  g_assert (MKS_IS_REMOTE_CONTENT_PROVIDER (provider));
  g_assert (G_IS_TASK (result));

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
mks_remote_content_provider_dispose (GObject *object)
{
  MksRemoteContentProvider *self = (MksRemoteContentProvider *)object;

  g_clear_object (&self->clipboard);
  g_clear_pointer (&self->mime_types, g_strfreev);

  G_OBJECT_CLASS (mks_remote_content_provider_parent_class)->dispose (object);
}

static void
mks_remote_content_provider_class_init (MksRemoteContentProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkContentProviderClass *provider_class = GDK_CONTENT_PROVIDER_CLASS (klass);

  object_class->dispose = mks_remote_content_provider_dispose;
  object_class->set_property = mks_remote_content_provider_set_property;

  provider_class->ref_formats = mks_remote_content_provider_ref_formats;
  provider_class->ref_storable_formats = mks_remote_content_provider_ref_formats;
  provider_class->write_mime_type_async = mks_remote_content_provider_write_mime_type_async;
  provider_class->write_mime_type_finish = mks_remote_content_provider_write_mime_type_finish;

  g_object_class_install_property (object_class,
                                   1,
                                   g_param_spec_object ("clipboard", NULL, NULL,
                                                        MKS_TYPE_CLIPBOARD,
                                                        (G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (object_class,
                                   2,
                                   g_param_spec_uint ("selection", NULL, NULL,
                                                      0,
                                                      MKS_CLIPBOARD_SELECTION_SECONDARY,
                                                      MKS_CLIPBOARD_SELECTION_CLIPBOARD,
                                                      (G_PARAM_WRITABLE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (object_class,
                                   3,
                                   g_param_spec_boxed ("mime-types", NULL, NULL,
                                                       G_TYPE_STRV,
                                                       (G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS)));
}

static void
mks_remote_content_provider_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  MksRemoteContentProvider *self = MKS_REMOTE_CONTENT_PROVIDER (object);

  switch (prop_id)
    {
    case 1:
      self->clipboard = g_value_dup_object (value);
      break;

    case 2:
      self->selection = g_value_get_uint (value);
      break;

    case 3:
      self->mime_types = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_remote_content_provider_init (MksRemoteContentProvider *self)
{
}

/**
 * mks_clipboard_redirector_new:
 * @clipboard: a `MksClipboard`
 * @display: a `GdkDisplay`
 *
 * Creates a redirector between @clipboard and the host GTK clipboards on
 * @display.
 *
 * Returns: (transfer full): a `MksClipboardRedirector`
 */
MksClipboardRedirector *
mks_clipboard_redirector_new (MksClipboard *clipboard,
                              GdkDisplay   *display)
{
  g_return_val_if_fail (MKS_IS_CLIPBOARD (clipboard), NULL);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  return g_object_new (MKS_TYPE_CLIPBOARD_REDIRECTOR,
                       "clipboard", clipboard,
                       "display", display,
                       NULL);
}

/**
 * mks_clipboard_redirector_get_clipboard:
 * @self: a `MksClipboardRedirector`
 *
 * Gets the clipboard being redirected.
 *
 * Returns: (transfer none): a `MksClipboard`
 */
MksClipboard *
mks_clipboard_redirector_get_clipboard (MksClipboardRedirector *self)
{
  g_return_val_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self), NULL);

  return self->clipboard;
}

/**
 * mks_clipboard_redirector_get_display:
 * @self: a `MksClipboardRedirector`
 *
 * Gets the display used for host clipboard access.
 *
 * Returns: (transfer none): a `GdkDisplay`
 */
GdkDisplay *
mks_clipboard_redirector_get_display (MksClipboardRedirector *self)
{
  g_return_val_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self), NULL);

  return self->display;
}

/**
 * mks_clipboard_redirector_get_enabled:
 * @self: a `MksClipboardRedirector`
 *
 * Gets if clipboard redirection is enabled.
 */
gboolean
mks_clipboard_redirector_get_enabled (MksClipboardRedirector *self)
{
  g_return_val_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self), FALSE);

  return self->enabled;
}

/**
 * mks_clipboard_redirector_set_enabled:
 * @self: a `MksClipboardRedirector`
 * @enabled: if clipboard redirection should be enabled
 *
 * Sets if clipboard redirection is enabled.
 */
void
mks_clipboard_redirector_set_enabled (MksClipboardRedirector *self,
                                      gboolean                enabled)
{
  g_return_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self));

  enabled = !!enabled;

  if (self->enabled != enabled)
    {
      self->enabled = enabled;
      mks_clipboard_redirector_update_registration (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ENABLED]);
    }
}

/**
 * mks_clipboard_redirector_get_selections:
 * @self: a `MksClipboardRedirector`
 *
 * Gets the selections that are redirected.
 */
MksClipboardRedirectorSelections
mks_clipboard_redirector_get_selections (MksClipboardRedirector *self)
{
  g_return_val_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self), 0);

  return self->selections;
}

/**
 * mks_clipboard_redirector_set_selections:
 * @self: a `MksClipboardRedirector`
 * @selections: the selections to redirect
 *
 * Sets the selections that are redirected.
 */
void
mks_clipboard_redirector_set_selections (MksClipboardRedirector           *self,
                                         MksClipboardRedirectorSelections  selections)
{
  g_return_if_fail (MKS_IS_CLIPBOARD_REDIRECTOR (self));

  selections &= (MKS_CLIPBOARD_REDIRECTOR_SELECTION_CLIPBOARD |
                 MKS_CLIPBOARD_REDIRECTOR_SELECTION_PRIMARY);

  if (self->selections != selections)
    {
      self->selections = selections;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SELECTIONS]);
    }
}

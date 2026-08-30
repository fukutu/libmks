/* mks-dbus-clipboard.c
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

#include "mks-transport-private.h"
#include "mks-dbus-transport.h"
#include "mks-dbus-clipboard-private.h"
#include "mks-enums.h"
#include "mks-qemu.h"
#include "mks-util-private.h"

#define N_SELECTIONS 3

typedef struct
{
  MksDBusClipboard       *self;
  MksClipboardSelection   selection;
  guint                   serial;
  char                  **mime_types;
} Claim;

struct _MksDBusClipboard
{
  MksClipboard parent_instance;

  MksTransport     *transport;
  MksQemuObject    *object;
  MksQemuClipboard *clipboard;
  MksQemuClipboard *skeleton;

  gboolean            registered;
  guint               serial[N_SELECTIONS];
  MksClipboardOwner   owner[N_SELECTIONS];
  char              **mime_types[N_SELECTIONS];

  MksClipboardReadFunc read_func;
  gpointer             read_func_data;
  GDestroyNotify       read_func_data_destroy;
};

struct _MksDBusClipboardClass
{
  MksClipboardClass parent_class;
};

G_DEFINE_FINAL_TYPE (MksDBusClipboard, mks_dbus_clipboard, MKS_TYPE_CLIPBOARD)

enum {
  PROP_0,
  PROP_REGISTERED,
  N_PROPS
};

enum {
  OWNER_CHANGED,
  CHANGED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

static gboolean            mks_dbus_clipboard_get_registered (MksClipboard          *clipboard);
static MksClipboardOwner   mks_dbus_clipboard_get_owner      (MksClipboard          *clipboard,
                                                              MksClipboardSelection  selection);
static char              **mks_dbus_clipboard_dup_mime_types (MksClipboard          *clipboard,
                                                              MksClipboardSelection  selection);
static DexFuture          *mks_dbus_clipboard_register       (MksClipboard          *clipboard);
static DexFuture          *mks_dbus_clipboard_unregister     (MksClipboard          *clipboard);
static DexFuture          *mks_dbus_clipboard_claim          (MksClipboard          *clipboard,
                                                              MksClipboardSelection  selection,
                                                              const char * const    *mime_types);
static DexFuture          *mks_dbus_clipboard_release        (MksClipboard          *clipboard,
                                                              MksClipboardSelection  selection);
static DexFuture          *mks_dbus_clipboard_request        (MksClipboard          *clipboard,
                                                              MksClipboardSelection  selection,
                                                              const char * const    *mime_types);
static void                mks_dbus_clipboard_set_read_func  (MksClipboard          *clipboard,
                                                              MksClipboardReadFunc   read_func,
                                                              gpointer               user_data,
                                                              GDestroyNotify         destroy);

static gboolean
selection_is_valid (MksClipboardSelection selection)
{
  return selection >= MKS_CLIPBOARD_SELECTION_CLIPBOARD &&
         selection <= MKS_CLIPBOARD_SELECTION_SECONDARY;
}

static gboolean
mime_types_equal (const char * const *a,
                  const char * const *b)
{
  if (a == NULL || b == NULL)
    return a == b;

  return g_strv_equal (a, b);
}

static void
claim_free (Claim *claim)
{
  g_clear_object (&claim->self);
  g_clear_pointer (&claim->mime_types, g_strfreev);
  g_free (claim);
}

static void
mks_dbus_clipboard_set_registered (MksDBusClipboard *self,
                                   gboolean          registered)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));

  registered = !!registered;

  if (self->registered != registered)
    {
      self->registered = registered;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_REGISTERED]);
    }
}

static void
mks_dbus_clipboard_set_owner (MksDBusClipboard      *self,
                              MksClipboardSelection  selection,
                              MksClipboardOwner      owner,
                              const char * const    *mime_types)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (selection_is_valid (selection));

  if (self->owner[selection] == owner &&
      mime_types_equal ((const char * const *) self->mime_types[selection], mime_types))
    return;

  self->owner[selection] = owner;
  g_strfreev (self->mime_types[selection]);
  self->mime_types[selection] = g_strdupv ((char **) mime_types);

  g_signal_emit (self, signals[OWNER_CHANGED], 0, selection);
  g_signal_emit (self, signals[CHANGED], 0, selection);
}

static gboolean
mks_dbus_clipboard_handle_register_cb (MksDBusClipboard      *self,
                                       GDBusMethodInvocation *invocation,
                                       MksQemuClipboard      *skeleton)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (MKS_QEMU_IS_CLIPBOARD (skeleton));

  for (guint i = 0; i < N_SELECTIONS; i++)
    {
      self->serial[i] = 0;
      mks_dbus_clipboard_set_owner (self, i, MKS_CLIPBOARD_OWNER_NONE, NULL);
    }

  mks_dbus_clipboard_set_registered (self, TRUE);
  mks_qemu_clipboard_complete_register (skeleton, invocation);

  return TRUE;
}

static gboolean
mks_dbus_clipboard_handle_unregister_cb (MksDBusClipboard      *self,
                                         GDBusMethodInvocation *invocation,
                                         MksQemuClipboard      *skeleton)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (MKS_QEMU_IS_CLIPBOARD (skeleton));

  for (guint i = 0; i < N_SELECTIONS; i++)
    mks_dbus_clipboard_set_owner (self, i, MKS_CLIPBOARD_OWNER_NONE, NULL);

  mks_dbus_clipboard_set_registered (self, FALSE);
  mks_qemu_clipboard_complete_unregister (skeleton, invocation);

  return TRUE;
}

static gboolean
mks_dbus_clipboard_handle_grab_cb (MksDBusClipboard      *self,
                                   GDBusMethodInvocation *invocation,
                                   guint                  selection,
                                   guint                  serial,
                                   const char * const    *mime_types,
                                   MksQemuClipboard      *skeleton)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (MKS_QEMU_IS_CLIPBOARD (skeleton));

  if (selection < N_SELECTIONS && serial > self->serial[selection])
    {
      self->serial[selection] = serial;
      mks_dbus_clipboard_set_owner (self, selection, MKS_CLIPBOARD_OWNER_REMOTE, mime_types);
    }

  mks_qemu_clipboard_complete_grab (skeleton, invocation);

  return TRUE;
}

static gboolean
mks_dbus_clipboard_handle_release_cb (MksDBusClipboard      *self,
                                      GDBusMethodInvocation *invocation,
                                      guint                  selection,
                                      MksQemuClipboard      *skeleton)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (MKS_QEMU_IS_CLIPBOARD (skeleton));

  if (selection < N_SELECTIONS && self->owner[selection] == MKS_CLIPBOARD_OWNER_REMOTE)
    mks_dbus_clipboard_set_owner (self, selection, MKS_CLIPBOARD_OWNER_NONE, NULL);

  mks_qemu_clipboard_complete_release (skeleton, invocation);

  return TRUE;
}

static DexFuture *
mks_dbus_clipboard_handle_request_complete_cb (DexFuture *future,
                                               gpointer   user_data)
{
  GDBusMethodInvocation *invocation = user_data;
  g_autoptr(MksClipboardContent) content = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  const GValue *value;
  GVariant *data;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  if (!(value = dex_future_get_value (future, &error)))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return dex_future_new_true ();
    }

  content = g_value_dup_boxed (value);
  bytes = mks_clipboard_content_dup_bytes (content);
  data = g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), bytes, TRUE);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s@ay)",
                                                        mks_clipboard_content_get_mime_type (content),
                                                        data));

  return dex_future_new_true ();
}

static gboolean
mks_dbus_clipboard_handle_request_cb (MksDBusClipboard      *self,
                                      GDBusMethodInvocation *invocation,
                                      guint                  selection,
                                      const char * const    *mime_types,
                                      MksQemuClipboard      *skeleton)
{
  DexFuture *future;

  g_assert (MKS_IS_DBUS_CLIPBOARD (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (MKS_QEMU_IS_CLIPBOARD (skeleton));

  if (selection >= N_SELECTIONS || self->read_func == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             "Clipboard content is not available");
      return TRUE;
    }

  future = self->read_func (MKS_CLIPBOARD (self), selection, mime_types, self->read_func_data);
  dex_future_disown (dex_future_finally (future,
                                         mks_dbus_clipboard_handle_request_complete_cb,
                                         g_object_ref (invocation),
                                         g_object_unref));

  return TRUE;
}

static gboolean
mks_dbus_clipboard_export_skeleton (MksDBusClipboard  *self,
                                    GError           **error)
{
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));

  if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
    return TRUE;

  return g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                           mks_dbus_transport_get_connection (MKS_DBUS_TRANSPORT (self->transport)),
                                           g_dbus_proxy_get_object_path (G_DBUS_PROXY (self->clipboard)),
                                           error);
}

static DexFuture *
mks_dbus_clipboard_complete_boolean (DexFuture *future,
                                     gpointer   user_data)
{
  g_autoptr(GError) error = NULL;

  g_assert (DEX_IS_FUTURE (future));

  if (!dex_future_get_value (future, &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  return dex_future_new_true ();
}

static DexFuture *
mks_dbus_clipboard_register_complete_cb (DexFuture *future,
                                         gpointer   user_data)
{
  MksDBusClipboard *self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));

  if (!dex_future_get_value (future, &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  for (guint i = 0; i < N_SELECTIONS; i++)
    {
      self->serial[i] = 0;
      mks_dbus_clipboard_set_owner (self, i, MKS_CLIPBOARD_OWNER_NONE, NULL);
    }

  mks_dbus_clipboard_set_registered (self, TRUE);

  return dex_future_new_true ();
}

static DexFuture *
mks_dbus_clipboard_unregister_complete_cb (DexFuture *future,
                                           gpointer   user_data)
{
  MksDBusClipboard *self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (MKS_IS_DBUS_CLIPBOARD (self));

  if (!dex_future_get_value (future, &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  for (guint i = 0; i < N_SELECTIONS; i++)
    mks_dbus_clipboard_set_owner (self, i, MKS_CLIPBOARD_OWNER_NONE, NULL);

  mks_dbus_clipboard_set_registered (self, FALSE);

  return dex_future_new_true ();
}

static DexFuture *
mks_dbus_clipboard_claim_complete_cb (DexFuture *future,
                                      gpointer   user_data)
{
  Claim *claim = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (claim != NULL);

  if (!dex_future_get_value (future, &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  mks_dbus_clipboard_set_owner (claim->self,
                           claim->selection,
                           MKS_CLIPBOARD_OWNER_LOCAL,
                           (const char * const *) claim->mime_types);

  return dex_future_new_true ();
}

static DexFuture *
mks_dbus_clipboard_request_complete_cb (DexFuture *future,
                                        gpointer   user_data)
{
  g_autoptr(MksClipboardContent) content = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  const GValue *value;
  MksQemuClipboardRequestResult *result;
  GValue boxed = G_VALUE_INIT;
  DexFuture *ret;

  g_assert (DEX_IS_FUTURE (future));

  if (!(value = dex_future_get_value (future, &error)))
    return dex_future_new_for_error (g_steal_pointer (&error));

  result = g_value_get_boxed (value);
  bytes = g_variant_get_data_as_bytes (result->data);
  content = mks_clipboard_content_new (result->reply_mime, bytes);

  g_value_init (&boxed, MKS_TYPE_CLIPBOARD_CONTENT);
  g_value_set_boxed (&boxed, content);
  ret = dex_future_new_for_value (&boxed);
  g_value_unset (&boxed);

  return ret;
}

static void
mks_dbus_clipboard_dispose (GObject *object)
{
  MksDBusClipboard *self = (MksDBusClipboard *)object;

  mks_dbus_clipboard_set_read_func (MKS_CLIPBOARD (self), NULL, NULL, NULL);

  if (self->skeleton != NULL)
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));

  g_clear_object (&self->skeleton);
  g_clear_object (&self->clipboard);
  g_clear_object (&self->object);
  g_clear_object (&self->transport);

  for (guint i = 0; i < N_SELECTIONS; i++)
    g_clear_pointer (&self->mime_types[i], g_strfreev);

  G_OBJECT_CLASS (mks_dbus_clipboard_parent_class)->dispose (object);
}

static void
mks_dbus_clipboard_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (object);

  switch (prop_id)
    {
    case PROP_REGISTERED:
      g_value_set_boolean (value, mks_dbus_clipboard_get_registered (MKS_CLIPBOARD (self)));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_dbus_clipboard_class_init (MksDBusClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MksClipboardClass *clipboard_class = MKS_CLIPBOARD_CLASS (klass);

  object_class->dispose = mks_dbus_clipboard_dispose;
  object_class->get_property = mks_dbus_clipboard_get_property;

  clipboard_class->get_registered = mks_dbus_clipboard_get_registered;
  clipboard_class->get_owner = mks_dbus_clipboard_get_owner;
  clipboard_class->dup_mime_types = mks_dbus_clipboard_dup_mime_types;
  clipboard_class->register_ = mks_dbus_clipboard_register;
  clipboard_class->unregister = mks_dbus_clipboard_unregister;
  clipboard_class->claim = mks_dbus_clipboard_claim;
  clipboard_class->release = mks_dbus_clipboard_release;
  clipboard_class->request = mks_dbus_clipboard_request;
  clipboard_class->set_read_func = mks_dbus_clipboard_set_read_func;

  properties[PROP_REGISTERED] =
    g_param_spec_boolean ("registered", NULL, NULL,
                          FALSE,
                          (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[OWNER_CHANGED] =
    g_signal_new ("owner-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  MKS_TYPE_CLIPBOARD_SELECTION);

  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  MKS_TYPE_CLIPBOARD_SELECTION);
}

static void
mks_dbus_clipboard_init (MksDBusClipboard *self)
{
}

MksClipboard *
_mks_dbus_clipboard_new (MksTransport  *transport,
                         MksQemuObject *object)
{
  MksDBusClipboard *self;

  g_return_val_if_fail (MKS_IS_TRANSPORT (transport), NULL);
  g_return_val_if_fail (MKS_QEMU_IS_OBJECT (object), NULL);

  self = g_object_new (MKS_TYPE_DBUS_CLIPBOARD, NULL);
  self->transport = g_object_ref (transport);
  self->object = g_object_ref (object);
  self->clipboard = mks_qemu_object_get_clipboard (object);
  self->skeleton = mks_qemu_clipboard_skeleton_new ();

  g_signal_connect_object (self->skeleton,
                           "handle-register",
                           G_CALLBACK (mks_dbus_clipboard_handle_register_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->skeleton,
                           "handle-unregister",
                           G_CALLBACK (mks_dbus_clipboard_handle_unregister_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->skeleton,
                           "handle-grab",
                           G_CALLBACK (mks_dbus_clipboard_handle_grab_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->skeleton,
                           "handle-release",
                           G_CALLBACK (mks_dbus_clipboard_handle_release_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->skeleton,
                           "handle-request",
                           G_CALLBACK (mks_dbus_clipboard_handle_request_cb),
                           self,
                           G_CONNECT_SWAPPED);

  return MKS_CLIPBOARD (self);
}

static void
mks_dbus_clipboard_set_read_func (MksClipboard         *clipboard,
                                  MksClipboardReadFunc  read_func,
                                  gpointer              user_data,
                                  GDestroyNotify        destroy)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  g_return_if_fail (MKS_IS_DBUS_CLIPBOARD (self));

  if (self->read_func_data_destroy != NULL)
    self->read_func_data_destroy (self->read_func_data);

  self->read_func = read_func;
  self->read_func_data = user_data;
  self->read_func_data_destroy = destroy;
}

/**
 * mks_dbus_clipboard_get_registered:
 * @self: a `MksDBusClipboard`
 *
 * Gets if @self has registered with QEMU for clipboard redirection.
 */
static gboolean
mks_dbus_clipboard_get_registered (MksClipboard *clipboard)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  g_return_val_if_fail (MKS_IS_DBUS_CLIPBOARD (self), FALSE);

  return self->registered;
}

/**
 * mks_dbus_clipboard_get_owner:
 * @self: a `MksDBusClipboard`
 * @selection: the clipboard selection
 *
 * Gets the current owner for @selection.
 */
static MksClipboardOwner
mks_dbus_clipboard_get_owner (MksClipboard          *clipboard,
                              MksClipboardSelection  selection)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  g_return_val_if_fail (MKS_IS_DBUS_CLIPBOARD (self), MKS_CLIPBOARD_OWNER_NONE);
  g_return_val_if_fail (selection_is_valid (selection), MKS_CLIPBOARD_OWNER_NONE);

  return self->owner[selection];
}

/**
 * mks_dbus_clipboard_dup_mime_types:
 * @self: a `MksDBusClipboard`
 * @selection: the clipboard selection
 *
 * Gets the MIME types available for @selection.
 *
 * Returns: (transfer full) (nullable): the MIME types
 */
static char **
mks_dbus_clipboard_dup_mime_types (MksClipboard          *clipboard,
                                   MksClipboardSelection  selection)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  g_return_val_if_fail (MKS_IS_DBUS_CLIPBOARD (self), NULL);
  g_return_val_if_fail (selection_is_valid (selection), NULL);

  return g_strdupv (self->mime_types[selection]);
}

/**
 * mks_dbus_clipboard_register:
 * @self: a `MksDBusClipboard`
 *
 * Registers for clipboard redirection with QEMU.
 *
 * Returns: (transfer full): a future that resolves to %TRUE
 */
static DexFuture *
mks_dbus_clipboard_register (MksClipboard *clipboard)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);
  g_autoptr(GError) error = NULL;

  dex_return_error_if_fail (MKS_IS_DBUS_CLIPBOARD (self));

  if (!mks_dbus_clipboard_export_skeleton (self, &error))
    return dex_future_new_for_error (g_steal_pointer (&error));

  return dex_future_then (mks_qemu_clipboard_call_register_future (self->clipboard),
                          mks_dbus_clipboard_register_complete_cb,
                          g_object_ref (self),
                          g_object_unref);
}

/**
 * mks_dbus_clipboard_unregister:
 * @self: a `MksDBusClipboard`
 *
 * Unregisters clipboard redirection with QEMU.
 *
 * Returns: (transfer full): a future that resolves to %TRUE
 */
static DexFuture *
mks_dbus_clipboard_unregister (MksClipboard *clipboard)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  dex_return_error_if_fail (MKS_IS_DBUS_CLIPBOARD (self));

  return dex_future_then (mks_qemu_clipboard_call_unregister_future (self->clipboard),
                          mks_dbus_clipboard_unregister_complete_cb,
                          g_object_ref (self),
                          g_object_unref);
}

/**
 * mks_dbus_clipboard_claim:
 * @self: a `MksDBusClipboard`
 * @selection: the clipboard selection
 * @mime_types: (array zero-terminated=1): the available MIME types
 *
 * Claims @selection for the local peer.
 *
 * Returns: (transfer full): a future that resolves to %TRUE
 */
static DexFuture *
mks_dbus_clipboard_claim (MksClipboard          *clipboard,
                          MksClipboardSelection  selection,
                          const char * const    *mime_types)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);
  Claim *claim;

  dex_return_error_if_fail (MKS_IS_DBUS_CLIPBOARD (self));
  dex_return_error_if_fail (selection_is_valid (selection));
  dex_return_error_if_fail (mime_types != NULL);

  claim = g_new0 (Claim, 1);
  claim->self = g_object_ref (self);
  claim->selection = selection;
  claim->serial = ++self->serial[selection];
  claim->mime_types = g_strdupv ((char **) mime_types);

  return dex_future_then (mks_qemu_clipboard_call_grab_future (self->clipboard,
                                                               selection,
                                                               claim->serial,
                                                               mime_types),
                          mks_dbus_clipboard_claim_complete_cb,
                          claim,
                          (GDestroyNotify) claim_free);
}

/**
 * mks_dbus_clipboard_release:
 * @self: a `MksDBusClipboard`
 * @selection: the clipboard selection
 *
 * Releases @selection.
 *
 * Returns: (transfer full): a future that resolves to %TRUE
 */
static DexFuture *
mks_dbus_clipboard_release (MksClipboard          *clipboard,
                            MksClipboardSelection  selection)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  dex_return_error_if_fail (MKS_IS_DBUS_CLIPBOARD (self));
  dex_return_error_if_fail (selection_is_valid (selection));

  if (self->owner[selection] == MKS_CLIPBOARD_OWNER_LOCAL)
    mks_dbus_clipboard_set_owner (self, selection, MKS_CLIPBOARD_OWNER_NONE, NULL);

  return dex_future_then (mks_qemu_clipboard_call_release_future (self->clipboard, selection),
                          mks_dbus_clipboard_complete_boolean,
                          NULL,
                          NULL);
}

/**
 * mks_dbus_clipboard_request:
 * @self: a `MksDBusClipboard`
 * @selection: the clipboard selection
 * @mime_types: (array zero-terminated=1): MIME types in preference order
 *
 * Requests clipboard content from QEMU.
 *
 * Returns: (transfer full): a future that resolves to `MksClipboardContent`
 */
static DexFuture *
mks_dbus_clipboard_request (MksClipboard          *clipboard,
                            MksClipboardSelection  selection,
                            const char * const    *mime_types)
{
  MksDBusClipboard *self = MKS_DBUS_CLIPBOARD (clipboard);

  dex_return_error_if_fail (MKS_IS_DBUS_CLIPBOARD (self));
  dex_return_error_if_fail (selection_is_valid (selection));
  dex_return_error_if_fail (mime_types != NULL);

  return dex_future_then (mks_qemu_clipboard_call_request_future (self->clipboard,
                                                                  selection,
                                                                  mime_types),
                          mks_dbus_clipboard_request_complete_cb,
                          NULL,
                          NULL);
}

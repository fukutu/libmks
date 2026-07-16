/* mks-dbus-speaker.c
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

#include <glib-unix.h>
#include <gst/app/gstappsrc.h>

#include "mks-audio-format.h"
#include "mks-device-private.h"
#include "mks-qemu.h"
#include "mks-dbus-speaker-private.h"
#include "mks-util-private.h"

/**
 * MksDBusSpeaker:
 *
 * A virtualized QEMU speaker.
 */

struct _MksDBusSpeaker
{
  MksSpeaker               parent_instance;
  MksQemuAudio            *audio;
  MksQemuAudioOutListener *listener;
  GDBusConnection         *connection;
  GHashTable              *streams;
  GArray                  *pcm_observers;
  guint                    next_pcm_observer_id;
  guint                    dispatching_pcm : 1;
  guint                    muted : 1;
};

struct _MksDBusSpeakerClass
{
  MksSpeakerClass parent_class;
};

G_DEFINE_FINAL_TYPE (MksDBusSpeaker, mks_dbus_speaker, MKS_TYPE_SPEAKER)

enum {
  PROP_0,
  PROP_MUTED,
  N_PROPS
};

enum {
  STREAM_ADDED,
  STREAM_REMOVED,
  STREAM_ENABLED,
  VOLUME_CHANGED,
  N_SIGNALS
};

static GParamSpec *properties [N_PROPS];
static guint signals [N_SIGNALS];

static gboolean        mks_dbus_speaker_get_muted           (MksSpeaker        *speaker);
static void            mks_dbus_speaker_set_muted           (MksSpeaker        *speaker,
                                                             gboolean           muted);
static MksAudioFormat *mks_dbus_speaker_dup_format          (MksSpeaker        *speaker,
                                                             guint64            stream_id);
static guint           mks_dbus_speaker_add_pcm_observer    (MksSpeaker        *speaker,
                                                             MksSpeakerPcmFunc  callback,
                                                             gpointer           user_data,
                                                             GDestroyNotify     user_data_destroy);
static void            mks_dbus_speaker_remove_pcm_observer (MksSpeaker        *speaker,
                                                             guint              observer_id);
static GstElement     *mks_dbus_speaker_create_gst_source   (MksSpeaker        *speaker,
                                                             guint64            stream_id);


typedef struct _MksDBusSpeakerStream
{
  guint64         id;
  MksAudioFormat *format;
  guint           enabled : 1;
  guint           muted : 1;
} MksDBusSpeakerStream;

typedef struct _MksDBusSpeakerGstSource
{
  MksDBusSpeaker *speaker;
  guint64         stream_id;
  gulong          stream_added_handler;
  gulong          stream_removed_handler;
  gulong          stream_enabled_handler;
  guint           pcm_observer_id;
} MksDBusSpeakerGstSource;

typedef struct _PcmObserver
{
  guint             id;
  MksSpeakerPcmFunc callback;
  gpointer          user_data;
  GDestroyNotify    user_data_destroy;
} PcmObserver;

static void
mks_dbus_speaker_clear_pcm_observer (PcmObserver *observer)
{
  g_assert (observer != NULL);

  if (observer->user_data_destroy != NULL)
    {
      observer->user_data_destroy (observer->user_data);
      observer->user_data_destroy = NULL;
    }
}

static void
mks_dbus_speaker_stream_free (gpointer data)
{
  MksDBusSpeakerStream *stream = data;

  if (stream == NULL)
    return;

  g_clear_pointer (&stream->format, mks_audio_format_free);
  g_free (stream);
}

static void
mks_dbus_speaker_gst_source_free (gpointer data)
{
  MksDBusSpeakerGstSource *source = data;

  if (source == NULL)
    return;

  if (source->speaker != NULL)
    {
      if (source->pcm_observer_id != 0)
        {
          mks_dbus_speaker_remove_pcm_observer (MKS_SPEAKER (source->speaker), source->pcm_observer_id);
          source->pcm_observer_id = 0;
        }

      g_clear_signal_handler (&source->stream_added_handler, source->speaker);
      g_clear_signal_handler (&source->stream_removed_handler, source->speaker);
      g_clear_signal_handler (&source->stream_enabled_handler, source->speaker);
      g_clear_object (&source->speaker);
    }

  g_free (source);
}

static MksDBusSpeakerStream *
mks_dbus_speaker_lookup_stream (MksDBusSpeaker *self,
                                guint64         id)
{
  g_assert (MKS_IS_DBUS_SPEAKER (self));

  return g_hash_table_lookup (self->streams, &id);
}

static MksDBusSpeakerStream *
mks_dbus_speaker_ensure_stream (MksDBusSpeaker *self,
                                guint64         id,
                                MksAudioFormat *format)
{
  MksDBusSpeakerStream *stream;
  guint64 *stream_id;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (format != NULL);

  if ((stream = mks_dbus_speaker_lookup_stream (self, id)))
    {
      g_clear_pointer (&stream->format, mks_audio_format_free);
      stream->format = mks_audio_format_copy (format);
      return stream;
    }

  stream = g_new0 (MksDBusSpeakerStream, 1);
  stream->id = id;
  stream->format = mks_audio_format_copy (format);
  stream_id = g_new (guint64, 1);
  *stream_id = id;
  g_hash_table_insert (self->streams, stream_id, stream);

  return stream;
}

static void
mks_dbus_speaker_emit_pcm (MksDBusSpeaker *self,
                           guint64         stream_id,
                           GBytes         *bytes)
{
  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (bytes != NULL);

  if (self->pcm_observers->len == 0)
    return;

  g_assert (!self->dispatching_pcm);

  self->dispatching_pcm = TRUE;
  for (guint i = 0; i < self->pcm_observers->len; i++)
    {
      PcmObserver *observer = &g_array_index (self->pcm_observers, PcmObserver, i);

      observer->callback (MKS_SPEAKER (self), stream_id, bytes, observer->user_data);
    }
  self->dispatching_pcm = FALSE;
}

static gboolean
mks_dbus_speaker_handle_init (MksDBusSpeaker        *self,
                              GDBusMethodInvocation *invocation,
                              guint64                id,
                              guchar                 bits,
                              gboolean               is_signed,
                              gboolean               is_float,
                              guint                  freq,
                              guchar                 nchannels,
                              guint                  bytes_per_frame,
                              guint                  bytes_per_second,
                              gboolean               be)
{
  g_autoptr(MksAudioFormat) format = NULL;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  format = mks_audio_format_new (bits,
                                 is_signed,
                                 is_float,
                                 freq,
                                 nchannels,
                                 bytes_per_frame,
                                 bytes_per_second,
                                 be);

  mks_dbus_speaker_ensure_stream (self, id, format);
  g_signal_emit (self, signals [STREAM_ADDED], 0, id, format);
  mks_qemu_audio_out_listener_complete_init (self->listener, invocation);

  return TRUE;
}

static gboolean
mks_dbus_speaker_handle_fini (MksDBusSpeaker        *self,
                              GDBusMethodInvocation *invocation,
                              guint64                id)
{
  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  g_hash_table_remove (self->streams, &id);
  g_signal_emit (self, signals [STREAM_REMOVED], 0, id);
  mks_qemu_audio_out_listener_complete_fini (self->listener, invocation);

  return TRUE;
}

static gboolean
mks_dbus_speaker_handle_set_enabled (MksDBusSpeaker        *self,
                                     GDBusMethodInvocation *invocation,
                                     guint64                id,
                                     gboolean               enabled)
{
  MksDBusSpeakerStream *stream;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  if ((stream = mks_dbus_speaker_lookup_stream (self, id)))
    stream->enabled = !!enabled;

  g_signal_emit (self, signals [STREAM_ENABLED], 0, id, enabled);
  mks_qemu_audio_out_listener_complete_set_enabled (self->listener, invocation);

  return TRUE;
}

static gboolean
mks_dbus_speaker_handle_set_volume (MksDBusSpeaker        *self,
                                    GDBusMethodInvocation *invocation,
                                    guint64                id,
                                    gboolean               mute,
                                    GVariant              *volume)
{
  MksDBusSpeakerStream *stream;
  g_autoptr(GBytes) bytes = NULL;
  gconstpointer element_data;
  gsize n_elements;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  element_data = g_variant_get_fixed_array (volume, &n_elements, sizeof (guchar));
  bytes = g_bytes_new (element_data, n_elements);

  if ((stream = mks_dbus_speaker_lookup_stream (self, id)))
    stream->muted = !!mute;

  g_signal_emit (self, signals [VOLUME_CHANGED], 0, id, mute, bytes);
  mks_qemu_audio_out_listener_complete_set_volume (self->listener, invocation);

  return TRUE;
}

static gboolean
mks_dbus_speaker_handle_write (MksDBusSpeaker        *self,
                               GDBusMethodInvocation *invocation,
                               guint64                id,
                               GVariant              *data)
{
  MksDBusSpeakerStream *stream;
  g_autoptr(GBytes) bytes = NULL;
  gconstpointer element_data;
  gsize n_elements;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));

  stream = mks_dbus_speaker_lookup_stream (self, id);

  if (!self->muted && (stream == NULL || !stream->muted))
    {
      element_data = g_variant_get_fixed_array (data, &n_elements, sizeof (guchar));
      bytes = g_bytes_new (element_data, n_elements);
      mks_dbus_speaker_emit_pcm (self, id, bytes);
    }

  mks_qemu_audio_out_listener_complete_write (self->listener, invocation);

  return TRUE;
}

static DexFuture *
mks_dbus_speaker_register_cb (DexFuture *future,
                              gpointer   user_data)
{
  MksDBusSpeaker *self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (MKS_IS_DBUS_SPEAKER (self));

  if (!dex_future_get_value (future, &error))
    g_warning ("Failed to register audio out listener: %s", error->message);

  return dex_future_new_true ();
}

static DexFuture *
mks_dbus_speaker_connection_cb (DexFuture *future,
                                gpointer   user_data)
{
  MksDBusSpeaker *self = user_data;
  g_autoptr(GError) error = NULL;
  const GValue *value;

  g_assert (DEX_IS_FUTURE (future));
  g_assert (MKS_IS_DBUS_SPEAKER (self));

  if (!(value = dex_future_get_value (future, &error)))
    {
      g_warning ("Failed to create audio out listener D-Bus connection: %s",
                 error->message);
      return dex_future_new_true ();
    }

  g_set_object (&self->connection, g_value_get_object (value));

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->listener),
                                         self->connection,
                                         "/org/qemu/Display1/AudioOutListener",
                                         &error))
    {
      g_warning ("Failed to export AudioOutListener on D-Bus connection: %s",
                 error->message);
      return dex_future_new_true ();
    }

  g_dbus_connection_start_message_processing (self->connection);

  return dex_future_new_true ();
}

static gboolean
mks_dbus_speaker_setup (MksDevice *device,
                        GObject   *object)
{
  MksDBusSpeaker *self = (MksDBusSpeaker *)device;
  g_autoptr(MksQemuAudio) audio = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GSocket) socket = NULL;
  g_autoptr(GIOStream) io_stream = NULL;
  gint64 begin_time;
  g_autofd int us = -1;
  g_autofd int them = -1;

  g_assert (MKS_IS_DBUS_SPEAKER (self));
  g_assert (MKS_QEMU_IS_OBJECT (object));

  if (!(audio = mks_qemu_object_get_audio (MKS_QEMU_OBJECT (object))))
    return FALSE;

  g_set_object (&self->audio, audio);

  /* Create the raw socketpair: us = our dbus end, them = peer fd for QEMU */
  if (!mks_socketpair_create (&us, &them, &error) ||
      !(socket = g_socket_new_from_fd (us, &error)))
    {
      g_warning ("Failed to create audio out listener socket: %s", error->message);
      return TRUE;
    }
  us = -1;
  io_stream = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  self->listener = mks_qemu_audio_out_listener_skeleton_new ();

  g_signal_connect_object (self->listener,
                           "handle-init",
                           G_CALLBACK (mks_dbus_speaker_handle_init),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->listener,
                           "handle-fini",
                           G_CALLBACK (mks_dbus_speaker_handle_fini),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->listener,
                           "handle-set-enabled",
                           G_CALLBACK (mks_dbus_speaker_handle_set_enabled),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->listener,
                           "handle-set-volume",
                           G_CALLBACK (mks_dbus_speaker_handle_set_volume),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->listener,
                           "handle-write",
                           G_CALLBACK (mks_dbus_speaker_handle_write),
                           self,
                           G_CONNECT_SWAPPED);

  begin_time = MKS_TRACE_BEGIN_MARK ();
  dex_future_disown
    (dex_future_finally
       (mks_marked_future
          (mks_dbus_connection_new (io_stream,
                                    (G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING |
                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
                                    NULL),
           begin_time,
           "speaker.socketpair-connection"),
        mks_dbus_speaker_connection_cb,
        g_object_ref (self),
        g_object_unref));

  /* Send 'them' to QEMU immediately without waiting for auth to complete.
   * QEMU acts as the dbus server on that fd, which unblocks our auth. */
  fd_list = g_unix_fd_list_new_from_array (&them, 1), them = -1;
  begin_time = MKS_TRACE_BEGIN_MARK ();
  dex_future_disown
    (dex_future_finally
       (mks_marked_future
          (dex_dbus_connection_call_with_unix_fd_list
             (g_dbus_proxy_get_connection (G_DBUS_PROXY (self->audio)),
              g_dbus_proxy_get_name (G_DBUS_PROXY (self->audio)),
              g_dbus_proxy_get_object_path (G_DBUS_PROXY (self->audio)),
              "org.qemu.Display1.Audio",
              "RegisterOutListener",
              g_variant_new ("(h)", 0),
              G_VARIANT_TYPE ("()"),
              G_DBUS_CALL_FLAGS_NONE,
              -1,
              fd_list),
           begin_time,
           "speaker.register-out-listener"),
        mks_dbus_speaker_register_cb,
        g_object_ref (self),
        g_object_unref));

  return TRUE;
}

static void
mks_dbus_speaker_dispose (GObject *object)
{
  MksDBusSpeaker *self = (MksDBusSpeaker *)object;

  if (self->listener != NULL)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->listener));
      g_clear_object (&self->listener);
    }

  g_clear_object (&self->connection);
  g_clear_object (&self->audio);
  g_clear_pointer (&self->streams, g_hash_table_unref);
  g_clear_pointer (&self->pcm_observers, g_array_unref);

  G_OBJECT_CLASS (mks_dbus_speaker_parent_class)->dispose (object);
}

static void
mks_dbus_speaker_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (object);

  switch (prop_id)
    {
    case PROP_MUTED:
      g_value_set_boolean (value, mks_dbus_speaker_get_muted (MKS_SPEAKER (self)));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_dbus_speaker_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (object);

  switch (prop_id)
    {
    case PROP_MUTED:
      mks_dbus_speaker_set_muted (MKS_SPEAKER (self), g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mks_dbus_speaker_class_init (MksDBusSpeakerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MksDeviceClass *device_class = MKS_DEVICE_CLASS (klass);
  MksSpeakerClass *speaker_class = MKS_SPEAKER_CLASS (klass);

  speaker_class->get_muted = mks_dbus_speaker_get_muted;
  speaker_class->set_muted = mks_dbus_speaker_set_muted;
  speaker_class->dup_format = mks_dbus_speaker_dup_format;
  speaker_class->add_pcm_observer = mks_dbus_speaker_add_pcm_observer;
  speaker_class->remove_pcm_observer = mks_dbus_speaker_remove_pcm_observer;
  speaker_class->create_gst_source = mks_dbus_speaker_create_gst_source;


  object_class->dispose = mks_dbus_speaker_dispose;
  object_class->get_property = mks_dbus_speaker_get_property;
  object_class->set_property = mks_dbus_speaker_set_property;

  device_class->setup = mks_dbus_speaker_setup;

  /**
   * MksDBusSpeaker:muted:
   *
   * If audio received from the instance is dropped and
   * the remote sound device should attempt to be set as muted.
   */
  properties [PROP_MUTED] =
    g_param_spec_boolean ("muted", NULL, NULL,
                          FALSE,
                          (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * MksDBusSpeaker::stream-added:
   * @self: a `MksDBusSpeaker`
   * @stream_id: the QEMU audio stream id
   * @format: the PCM stream format
   *
   * Emitted when QEMU initializes a playback stream.
   */
  signals [STREAM_ADDED] =
    g_signal_new ("stream-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_UINT64, MKS_TYPE_AUDIO_FORMAT);

  /**
   * MksDBusSpeaker::stream-removed:
   * @self: a `MksDBusSpeaker`
   * @stream_id: the QEMU audio stream id
   *
   * Emitted when QEMU finishes a playback stream.
   */
  signals [STREAM_REMOVED] =
    g_signal_new ("stream-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT64);

  /**
   * MksDBusSpeaker::stream-enabled:
   * @self: a `MksDBusSpeaker`
   * @stream_id: the QEMU audio stream id
   * @enabled: if the stream is enabled
   *
   * Emitted when QEMU resumes or suspends a playback stream.
   */
  signals [STREAM_ENABLED] =
    g_signal_new ("stream-enabled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_UINT64, G_TYPE_BOOLEAN);

  /**
   * MksDBusSpeaker::volume-changed:
   * @self: a `MksDBusSpeaker`
   * @stream_id: the QEMU audio stream id
   * @muted: if the stream is muted
   * @volume: (type GLib.Bytes): per-channel QEMU volume values
   *
   * Emitted when QEMU updates stream volume.
   */
  signals [VOLUME_CHANGED] =
    g_signal_new ("volume-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3, G_TYPE_UINT64, G_TYPE_BOOLEAN, G_TYPE_BYTES);

}

static void
mks_dbus_speaker_init (MksDBusSpeaker *self)
{
  self->streams = g_hash_table_new_full (g_int64_hash,
                                         g_int64_equal,
                                         g_free,
                                         mks_dbus_speaker_stream_free);
  self->pcm_observers = g_array_new (FALSE, FALSE, sizeof (PcmObserver));
  g_array_set_clear_func (self->pcm_observers,
                          (GDestroyNotify)mks_dbus_speaker_clear_pcm_observer);
  self->next_pcm_observer_id = 1;
}

/**
 * mks_dbus_speaker_get_muted:
 * @self: a #MksDBusSpeaker
 *
 * Gets if the speaker is muted.
 *
 * Returns: %TRUE if the #MksDBusSpeaker is muted.
 */
static gboolean
mks_dbus_speaker_get_muted (MksSpeaker *speaker)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);

  g_return_val_if_fail (MKS_IS_DBUS_SPEAKER (self), FALSE);

  return self->muted;
}

/**
 * mks_dbus_speaker_set_muted:
 * @self: a #MksDBusSpeaker
 * @muted: if the speaker should be muted
 *
 * Mute or un-mute the speaker.
 */
static void
mks_dbus_speaker_set_muted (MksSpeaker *speaker,
                            gboolean    muted)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);

  g_return_if_fail (MKS_IS_DBUS_SPEAKER (self));

  muted = !!muted;

  if (self->muted != muted)
    {
      self->muted = muted;
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_MUTED]);
    }
}

/**
 * mks_dbus_speaker_dup_format:
 * @self: a `MksDBusSpeaker`
 * @stream_id: a QEMU audio stream id
 *
 * Gets the PCM format for @stream_id.
 *
 * Returns: (transfer full) (nullable): a copy of the stream format
 */
static MksAudioFormat *
mks_dbus_speaker_dup_format (MksSpeaker *speaker,
                             guint64     stream_id)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);
  MksDBusSpeakerStream *stream;

  g_return_val_if_fail (MKS_IS_DBUS_SPEAKER (self), NULL);

  if (!(stream = mks_dbus_speaker_lookup_stream (self, stream_id)))
    return NULL;

  return mks_audio_format_copy (stream->format);
}

/**
 * mks_dbus_speaker_add_pcm_observer:
 * @self: a `MksDBusSpeaker`
 * @callback: (scope notified) (closure user_data): callback for PCM data delivery
 * @user_data: user data for @callback
 * @user_data_destroy: (destroy user_data) (nullable): destroy notify for @user_data
 *
 * Adds an observer for PCM playback frames.
 *
 * Observers must not add or remove PCM observers while a PCM callback is being
 * dispatched.
 *
 * Returns: an observer id for use with [method@Mks.Speaker.remove_pcm_observer]
 */
static guint
mks_dbus_speaker_add_pcm_observer (MksSpeaker        *speaker,
                                   MksSpeakerPcmFunc  callback,
                                   gpointer           user_data,
                                   GDestroyNotify     user_data_destroy)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);
  PcmObserver observer;
  guint id;

  g_return_val_if_fail (MKS_IS_DBUS_SPEAKER (self), 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (!self->dispatching_pcm, 0);

  id = self->next_pcm_observer_id++;
  if (id == 0)
    id = self->next_pcm_observer_id++;

  observer.id = id;
  observer.callback = callback;
  observer.user_data = user_data;
  observer.user_data_destroy = user_data_destroy;

  g_array_append_val (self->pcm_observers, observer);

  return id;
}

/**
 * mks_dbus_speaker_remove_pcm_observer:
 * @self: a `MksDBusSpeaker`
 * @observer_id: an observer id returned by [method@Mks.Speaker.add_pcm_observer]
 *
 * Removes a PCM playback observer.
 *
 * Observers must not remove themselves or other observers while a PCM callback
 * is being dispatched.
 */
static void
mks_dbus_speaker_remove_pcm_observer (MksSpeaker *speaker,
                                      guint       observer_id)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);

  g_return_if_fail (MKS_IS_DBUS_SPEAKER (self));
  g_return_if_fail (observer_id != 0);
  g_return_if_fail (!self->dispatching_pcm);

  for (guint i = 0; i < self->pcm_observers->len; i++)
    {
      PcmObserver *observer = &g_array_index (self->pcm_observers, PcmObserver, i);

      if (observer->id == observer_id)
        {
          g_array_remove_index (self->pcm_observers, i);
          break;
        }
    }
}

static void
mks_dbus_speaker_gst_source_update_caps (GstElement     *element,
                                         MksAudioFormat *format,
                                         guint           n_samples)
{
  g_autoptr(GstCaps) caps = NULL;

  g_assert (GST_IS_APP_SRC (element));
  g_assert (format != NULL);

  if ((caps = mks_audio_format_to_gst_caps (format)))
    gst_app_src_set_caps (GST_APP_SRC (element), caps);

  if (n_samples > 0)
    {
      guint blocksize = n_samples
                        * mks_audio_format_get_channels (format)
                        * (mks_audio_format_get_bits (format) / 8 /* bits-per-byte */);
      g_object_set (element, "blocksize", blocksize, NULL);
    }
}

static void
mks_dbus_speaker_gst_source_stream_added_cb (GstElement     *element,
                                             guint64         stream_id,
                                             MksAudioFormat *format,
                                             MksDBusSpeaker *speaker)
{
  MksDBusSpeakerGstSource *source;

  g_assert (GST_IS_APP_SRC (element));
  g_assert (format != NULL);
  g_assert (MKS_IS_DBUS_SPEAKER (speaker));

  source = g_object_get_data (G_OBJECT (element), "MksDBusSpeakerGstSource");

  if (source != NULL && source->stream_id == stream_id)
    mks_dbus_speaker_gst_source_update_caps (element, format,
                                             mks_qemu_audio_get_nsamples (speaker->audio));
}

static void
mks_dbus_speaker_gst_source_stream_removed_cb (GstElement     *element,
                                               guint64         stream_id,
                                               MksDBusSpeaker *speaker)
{
  MksDBusSpeakerGstSource *source;

  g_assert (GST_IS_APP_SRC (element));
  g_assert (MKS_IS_DBUS_SPEAKER (speaker));

  source = g_object_get_data (G_OBJECT (element), "MksDBusSpeakerGstSource");

  if (source != NULL && source->stream_id == stream_id)
    gst_app_src_end_of_stream (GST_APP_SRC (element));
}

static void
mks_dbus_speaker_gst_source_stream_enabled_cb (GstElement     *element,
                                               guint64         stream_id,
                                               gboolean        enabled,
                                               MksDBusSpeaker *speaker)
{
  MksDBusSpeakerGstSource *source;

  g_assert (GST_IS_APP_SRC (element));
  g_assert (MKS_IS_DBUS_SPEAKER (speaker));

  source = g_object_get_data (G_OBJECT (element), "MksDBusSpeakerGstSource");

  if (source != NULL && source->stream_id == stream_id && !enabled)
    gst_app_src_end_of_stream (GST_APP_SRC (element));
}

static void
mks_dbus_speaker_gst_source_pcm_cb (MksSpeaker *speaker,
                                    guint64     stream_id,
                                    GBytes     *bytes,
                                    gpointer    user_data)
{
  GstElement *element = user_data;
  MksDBusSpeakerGstSource *source;
  g_autoptr(GstBuffer) buffer = NULL;

  g_assert (GST_IS_APP_SRC (element));
  g_assert (bytes != NULL);
  g_assert (MKS_IS_DBUS_SPEAKER (speaker));

  source = g_object_get_data (G_OBJECT (element), "MksDBusSpeakerGstSource");

  if (source == NULL || source->stream_id != stream_id)
    return;

  buffer = gst_buffer_new_wrapped_bytes (g_bytes_ref (bytes));
  gst_app_src_push_buffer (GST_APP_SRC (element), g_steal_pointer (&buffer));
}

/**
 * mks_dbus_speaker_create_gst_source:
 * @self: a `MksDBusSpeaker`
 * @stream_id: a QEMU audio stream id
 *
 * Creates a GStreamer `appsrc` for PCM frames from @stream_id.
 *
 * Returns: (transfer floating): a new `GstAppSrc`
 */
static GstElement *
mks_dbus_speaker_create_gst_source (MksSpeaker *speaker,
                                    guint64     stream_id)
{
  MksDBusSpeaker *self = MKS_DBUS_SPEAKER (speaker);
  g_autoptr(MksAudioFormat) format = NULL;
  MksDBusSpeakerGstSource *source;
  GstElement *element;

  g_return_val_if_fail (MKS_IS_DBUS_SPEAKER (self), NULL);

  element = gst_element_factory_make ("appsrc", NULL);
  g_return_val_if_fail (element != NULL, NULL);

  g_object_set (element,
                "format", GST_FORMAT_TIME,
                "is-live", TRUE,
                "do-timestamp", TRUE,
                NULL);

  if ((format = mks_dbus_speaker_dup_format (MKS_SPEAKER (self), stream_id)))
    mks_dbus_speaker_gst_source_update_caps (element, format,
                                             mks_qemu_audio_get_nsamples (self->audio));

  source = g_new0 (MksDBusSpeakerGstSource, 1);
  source->speaker = g_object_ref (self);
  source->stream_id = stream_id;
  source->stream_added_handler =
    g_signal_connect_object (self,
                             "stream-added",
                             G_CALLBACK (mks_dbus_speaker_gst_source_stream_added_cb),
                             element,
                             G_CONNECT_SWAPPED);
  source->stream_removed_handler =
    g_signal_connect_object (self,
                             "stream-removed",
                             G_CALLBACK (mks_dbus_speaker_gst_source_stream_removed_cb),
                             element,
                             G_CONNECT_SWAPPED);
  source->stream_enabled_handler =
    g_signal_connect_object (self,
                             "stream-enabled",
                             G_CALLBACK (mks_dbus_speaker_gst_source_stream_enabled_cb),
                             element,
                             G_CONNECT_SWAPPED);
  source->pcm_observer_id =
    mks_dbus_speaker_add_pcm_observer (MKS_SPEAKER (self),
                                  mks_dbus_speaker_gst_source_pcm_cb,
                                  element,
                                  NULL);

  g_object_set_data_full (G_OBJECT (element),
                          "MksDBusSpeakerGstSource",
                          source,
                          mks_dbus_speaker_gst_source_free);

  return element;
}

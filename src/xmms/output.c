/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2009 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

/**
 * @file
 * Output plugin helper
 */

#include <string.h>
#include <unistd.h>

#include "xmmspriv/xmms_output.h"
#include "xmmspriv/xmms_ringbuf.h"
#include "xmmspriv/xmms_plugin.h"
#include "xmmspriv/xmms_xform.h"
#include "xmmspriv/xmms_sample.h"
#include "xmmspriv/xmms_medialib.h"
#include "xmmspriv/xmms_outputplugin.h"
#include "xmms/xmms_log.h"
#include "xmms/xmms_ipc.h"
#include "xmms/xmms_object.h"
#include "xmms/xmms_config.h"

#define VOLUME_MAX_CHANNELS 128

typedef struct xmms_volume_map_St {
	const gchar **names;
	guint *values;
	guint num_channels;
	gboolean status;
} xmms_volume_map_t;

static gboolean xmms_output_format_set (xmms_output_t *output, xmms_stream_type_t *fmt);
static gpointer xmms_output_monitor_volume_thread (gpointer data);

static void xmms_output_start (xmms_output_t *output, xmms_error_t *err);
static void xmms_output_stop (xmms_output_t *output, xmms_error_t *err);
static void xmms_output_pause (xmms_output_t *output, xmms_error_t *err);
static void xmms_output_xform_kill (xmms_output_t *output, xmms_error_t *err);
static void xmms_output_seekms (xmms_output_t *output, guint32 ms, xmms_error_t *error);
static void xmms_output_seekms_rel (xmms_output_t *output, gint32 ms, xmms_error_t *error);
static void xmms_output_seeksamples (xmms_output_t *output, guint32 samples, xmms_error_t *error);
static void xmms_output_seeksamples_rel (xmms_output_t *output, gint32 samples, xmms_error_t *error);
static gint32 xmms_output_status (xmms_output_t *output, xmms_error_t *error);

typedef enum xmms_output_filler_state_E {
	FILLER_STOP,
	FILLER_RUN,
	FILLER_QUIT,
	FILLER_KILL,
	FILLER_SEEK,
} xmms_output_filler_state_t;

static void xmms_output_volume_set (xmms_output_t *output, const gchar *channel, guint volume, xmms_error_t *error);
static GTree *xmms_output_volume_get (xmms_output_t *output, xmms_error_t *error);
static void xmms_output_filler_state (xmms_output_t *output, xmms_output_filler_state_t state);
static void xmms_output_filler_state_nolock (xmms_output_t *output, xmms_output_filler_state_t state);

static void xmms_volume_map_init (xmms_volume_map_t *vl);
static void xmms_volume_map_free (xmms_volume_map_t *vl);
static void xmms_volume_map_copy (xmms_volume_map_t *src, xmms_volume_map_t *dst);
static GTree *xmms_volume_map_to_dict (xmms_volume_map_t *vl);

static gboolean xmms_output_status_set (xmms_output_t *output, gint status);
static gboolean set_plugin (xmms_output_t *output, xmms_output_plugin_t *plugin);

XMMS_CMD_DEFINE (start, xmms_output_start, xmms_output_t *, NONE, NONE, NONE);
XMMS_CMD_DEFINE (stop, xmms_output_stop, xmms_output_t *, NONE, NONE, NONE);
XMMS_CMD_DEFINE (pause, xmms_output_pause, xmms_output_t *, NONE, NONE, NONE);
XMMS_CMD_DEFINE (xform_kill, xmms_output_xform_kill, xmms_output_t *, NONE, NONE, NONE);
XMMS_CMD_DEFINE (playtime, xmms_output_playtime, xmms_output_t *, INT32, NONE, NONE);
XMMS_CMD_DEFINE (seekms, xmms_output_seekms, xmms_output_t *, NONE, INT32, NONE);
XMMS_CMD_DEFINE (seekms_rel, xmms_output_seekms_rel, xmms_output_t *, NONE, INT32, NONE);
XMMS_CMD_DEFINE (seeksamples, xmms_output_seeksamples, xmms_output_t *, NONE, INT32, NONE);
XMMS_CMD_DEFINE (seeksamples_rel, xmms_output_seeksamples_rel, xmms_output_t *, NONE, INT32, NONE);
XMMS_CMD_DEFINE (output_status, xmms_output_status, xmms_output_t *, INT32, NONE, NONE);
XMMS_CMD_DEFINE (currentid, xmms_output_current_id, xmms_output_t *, INT32, NONE, NONE);
XMMS_CMD_DEFINE (volume_set, xmms_output_volume_set, xmms_output_t *, NONE, STRING, INT32);
XMMS_CMD_DEFINE (volume_get, xmms_output_volume_get, xmms_output_t *, DICT, NONE, NONE);

/*
 * Type definitions
 */

/** @defgroup Output Output
  * @ingroup XMMSServer
  * @brief Output is responsible to put the decoded data on
  * the soundcard.
  * @{
  */

/*
 *
 * locking order: status_mutex > write_mutex
 *                filler_mutex
 *                playtime_mutex is leaflock.
 */

struct xmms_output_St {
	xmms_object_t object;

	xmms_output_plugin_t *plugin;
	gpointer plugin_data;

	/* */
	GMutex *playtime_mutex;
	guint played;
	guint played_time;
	xmms_medialib_entry_t current_entry;
	guint toskip;

	/* */
	GThread *filler_thread;
	GMutex *filler_mutex;

	GCond *filler_state_cond;
	xmms_output_filler_state_t filler_state;

	xmms_ringbuf_t *filler_buffer;
	guint32 filler_seek;
	gint filler_skip;

	/** Internal status, tells which state the
	    output really is in */
	GMutex *status_mutex;
	guint status;

	xmms_playlist_t *playlist;

	/** Supported formats */
	GList *format_list;
	/** Active format */
	xmms_stream_type_t *format;

	/**
	 * Number of bytes totaly written to output driver,
	 * this is only for statistics...
	 */
	guint64 bytes_written;

	/**
	 * How many times didn't we have enough data in the buffer?
	 */
	gint32 buffer_underruns;

	GThread *monitor_volume_thread;
	gboolean monitor_volume_running;
};

/** @} */

/*
 * Public functions
 */

gpointer
xmms_output_private_data_get (xmms_output_t *output)
{
	g_return_val_if_fail (output, NULL);
	g_return_val_if_fail (output->plugin, NULL);

	return output->plugin_data;
}

void
xmms_output_private_data_set (xmms_output_t *output, gpointer data)
{
	g_return_if_fail (output);
	g_return_if_fail (output->plugin);

	output->plugin_data = data;
}

void
xmms_output_stream_type_add (xmms_output_t *output, ...)
{
	xmms_stream_type_t *f;
	va_list ap;

	va_start (ap, output);
	f = xmms_stream_type_parse (ap);
	va_end (ap);

	g_return_if_fail (f);

	output->format_list = g_list_append (output->format_list, f);
}

void
update_playtime (xmms_output_t *output, int advance)
{
	guint buffersize = 0;

	g_mutex_lock (output->playtime_mutex);
	output->played += advance;
	g_mutex_unlock (output->playtime_mutex);

	buffersize = xmms_output_plugin_method_latency_get (output->plugin, output);

	if (output->played < buffersize) {
		buffersize = output->played;
	}

	g_mutex_lock (output->playtime_mutex);

	if (output->format) {
		guint ms = xmms_sample_bytes_to_ms (output->format,
		                                    output->played - buffersize);
		if ((ms / 100) != (output->played_time / 100)) {
			xmms_object_emit_f (XMMS_OBJECT (output),
			                    XMMS_IPC_SIGNAL_OUTPUT_PLAYTIME,
			                    XMMSV_TYPE_INT32,
			                    ms);
		}
		output->played_time = ms;

	}

	g_mutex_unlock (output->playtime_mutex);

}

void
xmms_output_set_error (xmms_output_t *output, xmms_error_t *error)
{
	g_return_if_fail (output);

	xmms_output_status_set (output, XMMS_PLAYBACK_STATUS_STOP);

	if (error) {
		xmms_log_error ("Output plugin %s reported error, '%s'",
		                xmms_plugin_shortname_get ((xmms_plugin_t *)output->plugin),
		                xmms_error_message_get (error));
	}
}

typedef struct {
	xmms_output_t *output;
	xmms_xform_t *chain;
	gboolean flush;
} xmms_output_song_changed_arg_t;

static void
song_changed_arg_free (void *data)
{
	xmms_output_song_changed_arg_t *arg = (xmms_output_song_changed_arg_t *)data;
	xmms_object_unref (arg->chain);
	g_free (arg);
}

static gboolean
song_changed (void *data)
{
	/* executes in the output thread; NOT the filler thread */
	xmms_output_song_changed_arg_t *arg = (xmms_output_song_changed_arg_t *)data;
	xmms_medialib_entry_t entry;

	entry = xmms_xform_entry_get (arg->chain);

	XMMS_DBG ("Running hotspot! Song changed!! %d", entry);

	arg->output->played = 0;
	arg->output->current_entry = entry;

	if (!xmms_output_format_set (arg->output, xmms_xform_outtype_get (arg->chain))) {
		XMMS_DBG ("Couldn't set format, stopping filler..");
		xmms_output_filler_state_nolock (arg->output, FILLER_STOP);
		xmms_ringbuf_set_eos (arg->output->filler_buffer, TRUE);
		return FALSE;
	}

	if (arg->flush)
		xmms_output_flush (arg->output);

	xmms_object_emit_f (XMMS_OBJECT (arg->output),
	                    XMMS_IPC_SIGNAL_OUTPUT_CURRENTID,
	                    XMMSV_TYPE_INT32,
	                    entry);

	return TRUE;
}

static gboolean
seek_done (void *data)
{
	xmms_output_t *output = (xmms_output_t *)data;

	g_mutex_lock (output->playtime_mutex);
	output->played = output->filler_seek * xmms_sample_frame_size_get (output->format);
	output->toskip = output->filler_skip * xmms_sample_frame_size_get (output->format);
	g_mutex_unlock (output->playtime_mutex);

	xmms_output_flush (output);
	return TRUE;
}

static void
xmms_output_filler_state_nolock (xmms_output_t *output, xmms_output_filler_state_t state)
{
	output->filler_state = state;
	g_cond_signal (output->filler_state_cond);
	if (state == FILLER_QUIT || state == FILLER_STOP || state == FILLER_KILL) {
		xmms_ringbuf_clear (output->filler_buffer);
	}
	if (state != FILLER_STOP) {
		xmms_ringbuf_set_eos (output->filler_buffer, FALSE);
	}
}

static void
xmms_output_filler_state (xmms_output_t *output, xmms_output_filler_state_t state)
{
	g_mutex_lock (output->filler_mutex);
	xmms_output_filler_state_nolock (output, state);
	g_mutex_unlock (output->filler_mutex);
}
static void
xmms_output_filler_seek_state (xmms_output_t *output, guint32 samples)
{
	g_mutex_lock (output->filler_mutex);
	output->filler_state = FILLER_SEEK;
	output->filler_seek = samples;
	g_cond_signal (output->filler_state_cond);
	g_mutex_unlock (output->filler_mutex);
}

static void *
xmms_output_filler (void *arg)
{
	xmms_output_t *output = (xmms_output_t *)arg;
	xmms_xform_t *chain = NULL;
	gboolean last_was_kill = FALSE;
	char buf[4096];
	xmms_error_t err;
	gint ret;

	xmms_error_reset (&err);

	g_mutex_lock (output->filler_mutex);
	while (output->filler_state != FILLER_QUIT) {
		if (output->filler_state == FILLER_STOP) {
			if (chain) {
				xmms_object_unref (chain);
				chain = NULL;
			}
			xmms_ringbuf_set_eos (output->filler_buffer, TRUE);
			g_cond_wait (output->filler_state_cond, output->filler_mutex);
			last_was_kill = FALSE;
			continue;
		}
		if (output->filler_state == FILLER_KILL) {
			if (chain) {
				xmms_object_unref (chain);
				chain = NULL;
				output->filler_state = FILLER_RUN;
				last_was_kill = TRUE;
			} else {
				output->filler_state = FILLER_STOP;
			}
			continue;
		}
		if (output->filler_state == FILLER_SEEK) {
			if (!chain) {
				XMMS_DBG ("Seek without chain, ignoring..");
				output->filler_state = FILLER_STOP;
				continue;
			}

			ret = xmms_xform_this_seek (chain, output->filler_seek, XMMS_XFORM_SEEK_SET, &err);
			if (ret == -1) {
				XMMS_DBG ("Seeking failed: %s", xmms_error_message_get (&err));
			} else {
				XMMS_DBG ("Seek ok! %d", ret);

				output->filler_skip = output->filler_seek - ret;
				if (output->filler_skip < 0) {
					XMMS_DBG ("Seeked %d samples too far! Updating position...",
					          -output->filler_skip);

					output->filler_skip = 0;
					output->filler_seek = ret;
				}

				xmms_ringbuf_clear (output->filler_buffer);
				xmms_ringbuf_hotspot_set (output->filler_buffer, seek_done, NULL, output);
			}
			output->filler_state = FILLER_RUN;
		}

		if (!chain) {
			xmms_medialib_entry_t entry;
			xmms_output_song_changed_arg_t *hsarg;
			xmms_medialib_session_t *session;

			g_mutex_unlock (output->filler_mutex);

			entry = xmms_playlist_current_entry (output->playlist);
			if (!entry) {
				XMMS_DBG ("No entry from playlist!");
				output->filler_state = FILLER_STOP;
				g_mutex_lock (output->filler_mutex);
				continue;
			}

			chain = xmms_xform_chain_setup (entry, output->format_list, FALSE);
			if (!chain) {
				session = xmms_medialib_begin_write ();
				if (xmms_medialib_entry_property_get_int (session, entry, XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS) == XMMS_MEDIALIB_ENTRY_STATUS_NEW) {
					xmms_medialib_end (session);
					xmms_medialib_entry_remove (entry);
				} else {
					xmms_medialib_entry_status_set (session, entry, XMMS_MEDIALIB_ENTRY_STATUS_NOT_AVAILABLE);
					xmms_medialib_entry_send_update (entry);
					xmms_medialib_end (session);
				}

				if (!xmms_playlist_advance (output->playlist)) {
					XMMS_DBG ("End of playlist");
					output->filler_state = FILLER_STOP;
				}
				g_mutex_lock (output->filler_mutex);
				continue;
			}

			hsarg = g_new0 (xmms_output_song_changed_arg_t, 1);
			hsarg->output = output;
			hsarg->chain = chain;
			hsarg->flush = last_was_kill;
			xmms_object_ref (chain);

			last_was_kill = FALSE;

			g_mutex_lock (output->filler_mutex);
			xmms_ringbuf_hotspot_set (output->filler_buffer, song_changed, song_changed_arg_free, hsarg);
		}

		xmms_ringbuf_wait_free (output->filler_buffer, sizeof (buf), output->filler_mutex);

		if (output->filler_state != FILLER_RUN) {
			XMMS_DBG ("State changed while waiting...");
			continue;
		}
		g_mutex_unlock (output->filler_mutex);

		ret = xmms_xform_this_read (chain, buf, sizeof (buf), &err);

		g_mutex_lock (output->filler_mutex);

		if (ret > 0) {
			gint skip = MIN (ret, output->toskip);

			output->toskip -= skip;
			if (ret > skip) {
				xmms_ringbuf_write_wait (output->filler_buffer,
				                         buf + skip,
				                         ret - skip,
				                         output->filler_mutex);
			}
		} else {
			if (ret == -1) {
				/* print error */
				xmms_error_reset (&err);
			}
			xmms_object_unref (chain);
			chain = NULL;
			if (!xmms_playlist_advance (output->playlist)) {
				XMMS_DBG ("End of playlist");
				output->filler_state = FILLER_STOP;
			}
		}

	}
	g_mutex_unlock (output->filler_mutex);
	return NULL;
}

gint
xmms_output_read (xmms_output_t *output, char *buffer, gint len)
{
	gint ret;
	xmms_error_t err;

	xmms_error_reset (&err);

	g_return_val_if_fail (output, -1);
	g_return_val_if_fail (buffer, -1);

	g_mutex_lock (output->filler_mutex);
	xmms_ringbuf_wait_used (output->filler_buffer, len, output->filler_mutex);
	ret = xmms_ringbuf_read (output->filler_buffer, buffer, len);
	if (ret == 0 && xmms_ringbuf_iseos (output->filler_buffer)) {
		xmms_output_status_set (output, XMMS_PLAYBACK_STATUS_STOP);
		g_mutex_unlock (output->filler_mutex);
		return -1;
	}
	g_mutex_unlock (output->filler_mutex);

	update_playtime (output, ret);

	if (ret < len) {
		XMMS_DBG ("Underrun %d of %d (%d)", ret, len, xmms_sample_frame_size_get (output->format));

		if ((ret % xmms_sample_frame_size_get (output->format)) != 0) {
			xmms_log_error ("***********************************");
			xmms_log_error ("* Read non-multiple of sample size,");
			xmms_log_error ("*  you probably hear noise now :)");
			xmms_log_error ("***********************************");
		}
		output->buffer_underruns++;
	}

	output->bytes_written += ret;

	return ret;
}

xmms_config_property_t *
xmms_output_config_property_register (xmms_output_t *output, const gchar *name, const gchar *default_value, xmms_object_handler_t cb, gpointer userdata)
{
	g_return_val_if_fail (output->plugin, NULL);
	return xmms_plugin_config_property_register ((xmms_plugin_t *)output->plugin, name, default_value, cb, userdata);
}

xmms_config_property_t *
xmms_output_config_lookup (xmms_output_t *output, const gchar *path)
{
	g_return_val_if_fail (output->plugin, NULL);
	return xmms_plugin_config_lookup ((xmms_plugin_t *)output->plugin, path);
}


/** @addtogroup Output
 * @{
 */
/** Methods */
static void
xmms_output_xform_kill (xmms_output_t *output, xmms_error_t *error)
{
	xmms_output_filler_state (output, FILLER_KILL);
}

static void
xmms_output_seekms (xmms_output_t *output, guint32 ms, xmms_error_t *error)
{
	g_return_if_fail (output);
	if (output->format) {
		xmms_output_seeksamples (output, xmms_sample_ms_to_samples (output->format, ms), error);
	}
}

static void
xmms_output_seekms_rel (xmms_output_t *output, gint32 ms, xmms_error_t *error)
{
	g_mutex_lock (output->playtime_mutex);
	ms += output->played_time;
	if (ms < 0) {
		ms = 0;
	}
	g_mutex_unlock (output->playtime_mutex);

	xmms_output_seekms (output, ms, error);
}

static void
xmms_output_seeksamples (xmms_output_t *output, guint32 samples, xmms_error_t *error)
{
	/* "just" tell filler */
	xmms_output_filler_seek_state (output, samples);
}

static void
xmms_output_seeksamples_rel (xmms_output_t *output, gint32 samples, xmms_error_t *error)
{
	g_mutex_lock (output->playtime_mutex);
	samples += output->played / xmms_sample_frame_size_get (output->format);
	if (samples < 0) {
		samples = 0;
	}
	g_mutex_unlock (output->playtime_mutex);

	xmms_output_seeksamples (output, samples, error);
}

static void
xmms_output_start (xmms_output_t *output, xmms_error_t *err)
{
	g_return_if_fail (output);

	xmms_output_filler_state (output, FILLER_RUN);
	if (!xmms_output_status_set (output, XMMS_PLAYBACK_STATUS_PLAY)) {
		xmms_output_filler_state (output, FILLER_STOP);
		xmms_error_set (err, XMMS_ERROR_GENERIC, "Could not start playback");
	}

}

static void
xmms_output_stop (xmms_output_t *output, xmms_error_t *err)
{
	g_return_if_fail (output);

	xmms_output_status_set (output, XMMS_PLAYBACK_STATUS_STOP);

	xmms_output_filler_state (output, FILLER_STOP);
}

static void
xmms_output_pause (xmms_output_t *output, xmms_error_t *err)
{
	g_return_if_fail (output);

	xmms_output_status_set (output, XMMS_PLAYBACK_STATUS_PAUSE);
}


static gint32
xmms_output_status (xmms_output_t *output, xmms_error_t *error)
{
	gint32 ret;
	g_return_val_if_fail (output, XMMS_PLAYBACK_STATUS_STOP);

	g_mutex_lock (output->status_mutex);
	ret = output->status;
	g_mutex_unlock (output->status_mutex);
	return ret;
}

gint
xmms_output_current_id (xmms_output_t *output, xmms_error_t *error)
{
	return output->current_entry;
}

static void
xmms_output_volume_set (xmms_output_t *output, const gchar *channel,
                        guint volume, xmms_error_t *error)
{

	if (!output->plugin) {
		xmms_error_set (error, XMMS_ERROR_GENERIC,
		                "couldn't set volume, output plugin not loaded");
		return;
	}

	if (!xmms_output_plugin_method_volume_set_available (output->plugin)) {
		xmms_error_set (error, XMMS_ERROR_GENERIC,
		                "operation not supported");
		return;
	}

	if (volume > 100) {
		xmms_error_set (error, XMMS_ERROR_INVAL, "volume out of range");
		return;
	}

	if (!xmms_output_plugin_methods_volume_set (output->plugin, output, channel, volume)) {
		xmms_error_set (error, XMMS_ERROR_GENERIC,
		                "couldn't set volume");
	}
}

static GTree *
xmms_output_volume_get (xmms_output_t *output, xmms_error_t *error)
{
	GTree *ret;
	xmms_volume_map_t map;

	if (!output->plugin) {
		xmms_error_set (error, XMMS_ERROR_GENERIC,
		                "couldn't get volume, output plugin not loaded");
		return NULL;
	}

	if (!xmms_output_plugin_method_volume_get_available (output->plugin)) {
		xmms_error_set (error, XMMS_ERROR_GENERIC,
		                "operation not supported");
		return NULL;
	}

	xmms_error_set (error, XMMS_ERROR_GENERIC,
	                "couldn't get volume");

	xmms_volume_map_init (&map);

	/* ask the plugin how much channels it would like to set */
	if (!xmms_output_plugin_method_volume_get (output->plugin, output,
	                                           NULL, NULL, &map.num_channels)) {
		return NULL;
	}

	/* check for sane values */
	g_return_val_if_fail (map.num_channels > 0, NULL);
	g_return_val_if_fail (map.num_channels <= VOLUME_MAX_CHANNELS, NULL);

	map.names = g_new (const gchar *, map.num_channels);
	map.values = g_new (guint, map.num_channels);

	map.status = xmms_output_plugin_method_volume_get (output->plugin, output,
	                                                   map.names, map.values,
	                                                   &map.num_channels);

	if (!map.status || !map.num_channels) {
		return NULL; /* error is set (-> no leak) */
	}

	ret = xmms_volume_map_to_dict (&map);

	/* success! */
	xmms_error_reset (error);

	return ret;
}

/**
 * Get the current playtime in milliseconds.
 */
gint32
xmms_output_playtime (xmms_output_t *output, xmms_error_t *error)
{
	guint32 ret;
	g_return_val_if_fail (output, 0);

	g_mutex_lock (output->playtime_mutex);
	ret = output->played_time;
	g_mutex_unlock (output->playtime_mutex);

	return ret;
}

/* returns the current latency: time left in ms until the data currently read
 *                              from the latest xform in the chain will actually be played
 */
guint32
xmms_output_latency (xmms_output_t *output)
{
	guint ret = 0;
	guint buffersize = 0;

	if (output->format) {
		/* data already waiting in the ringbuffer */
		buffersize += xmms_ringbuf_bytes_used (output->filler_buffer);

		/* latency of the soundcard */
		buffersize += xmms_output_plugin_method_latency_get (output->plugin, output);

		ret = xmms_sample_bytes_to_ms (output->format, buffersize);
	}

	return ret;
}

/**
 * @internal
 */

static gboolean
xmms_output_status_set (xmms_output_t *output, gint status)
{
	gboolean ret = TRUE;

	if (!output->plugin) {
		XMMS_DBG ("No plugin to set status on..");
		return FALSE;
	}

	g_mutex_lock (output->status_mutex);

	if (output->status != status) {
		if (status == XMMS_PLAYBACK_STATUS_PAUSE &&
		    output->status != XMMS_PLAYBACK_STATUS_PLAY) {
			XMMS_DBG ("Can only pause from play.");
			ret = FALSE;
		} else {
			output->status = status;

			if (status == XMMS_PLAYBACK_STATUS_STOP) {
				xmms_object_unref (output->format);
				output->format = NULL;
			}
			if (!xmms_output_plugin_method_status (output->plugin, output, status)) {
				xmms_log_error ("Status method returned an error!");
				output->status = XMMS_PLAYBACK_STATUS_STOP;
				ret = FALSE;
			}

			xmms_object_emit_f (XMMS_OBJECT (output),
			                    XMMS_IPC_SIGNAL_PLAYBACK_STATUS,
			                    XMMSV_TYPE_INT32,
			                    output->status);
		}
	}

	g_mutex_unlock (output->status_mutex);

	return ret;
}

static void
xmms_output_destroy (xmms_object_t *object)
{
	xmms_output_t *output = (xmms_output_t *)object;

	output->monitor_volume_running = FALSE;
	if (output->monitor_volume_thread) {
		g_thread_join (output->monitor_volume_thread);
		output->monitor_volume_thread = NULL;
	}

	xmms_output_filler_state (output, FILLER_QUIT);
	g_thread_join (output->filler_thread);

	if (output->plugin) {
		xmms_output_plugin_method_destroy (output->plugin, output);
		xmms_object_unref (output->plugin);
	}

	xmms_object_unref (output->playlist);

	g_mutex_free (output->status_mutex);
	g_mutex_free (output->playtime_mutex);
	g_mutex_free (output->filler_mutex);
	g_cond_free (output->filler_state_cond);
	xmms_ringbuf_destroy (output->filler_buffer);

	xmms_ipc_broadcast_unregister ( XMMS_IPC_SIGNAL_OUTPUT_VOLUME_CHANGED);
	xmms_ipc_broadcast_unregister ( XMMS_IPC_SIGNAL_PLAYBACK_STATUS);
	xmms_ipc_broadcast_unregister ( XMMS_IPC_SIGNAL_OUTPUT_CURRENTID);
	xmms_ipc_signal_unregister (XMMS_IPC_SIGNAL_OUTPUT_PLAYTIME);
	xmms_ipc_object_unregister (XMMS_IPC_OBJECT_OUTPUT);
}

/**
 * Switch to another output plugin.
 * @param output output pointer
 * @param new_plugin the new #xmms_plugin_t to use as output.
 * @returns TRUE on success and FALSE on failure
 */
gboolean
xmms_output_plugin_switch (xmms_output_t *output, xmms_output_plugin_t *new_plugin)
{
	xmms_output_plugin_t *old_plugin;
	gboolean ret;

	g_return_val_if_fail (output, FALSE);
	g_return_val_if_fail (new_plugin, FALSE);

	xmms_output_stop (output, NULL);

	g_mutex_lock (output->status_mutex);

	old_plugin = output->plugin;

	ret = set_plugin (output, new_plugin);

	/* if the switch succeeded, release the reference to the old plugin
	 * now.
	 * if we couldn't switch to the new plugin, but we had a working
	 * plugin before, switch back to the old plugin.
	 */
	if (ret) {
		xmms_object_unref (old_plugin);
	} else if (old_plugin) {
		XMMS_DBG ("cannot switch plugin, going back to old one");
		set_plugin (output, old_plugin);
	}

	g_mutex_unlock (output->status_mutex);

	return ret;
}

/**
 * Allocate a new #xmms_output_t
 */
xmms_output_t *
xmms_output_new (xmms_output_plugin_t *plugin, xmms_playlist_t *playlist)
{
	xmms_output_t *output;
	xmms_config_property_t *prop;
	gint size;

	g_return_val_if_fail (playlist, NULL);

	XMMS_DBG ("Trying to open output");

	output = xmms_object_new (xmms_output_t, xmms_output_destroy);

	output->playlist = playlist;

	output->status_mutex = g_mutex_new ();
	output->playtime_mutex = g_mutex_new ();

	prop = xmms_config_property_register ("output.buffersize", "32768", NULL, NULL);
	size = xmms_config_property_get_int (prop);
	XMMS_DBG ("Using buffersize %d", size);

	output->filler_mutex = g_mutex_new ();
	output->filler_state = FILLER_STOP;
	output->filler_state_cond = g_cond_new ();
	output->filler_buffer = xmms_ringbuf_new (size);
	output->filler_thread = g_thread_create (xmms_output_filler, output, TRUE, NULL);

	xmms_config_property_register ("output.flush_on_pause", "1", NULL, NULL);
	xmms_ipc_object_register (XMMS_IPC_OBJECT_OUTPUT, XMMS_OBJECT (output));

	/* Broadcasts are always transmitted to the client if he
	 * listens to them. */
	xmms_ipc_broadcast_register (XMMS_OBJECT (output),
	                             XMMS_IPC_SIGNAL_OUTPUT_VOLUME_CHANGED);
	xmms_ipc_broadcast_register (XMMS_OBJECT (output),
	                             XMMS_IPC_SIGNAL_PLAYBACK_STATUS);
	xmms_ipc_broadcast_register (XMMS_OBJECT (output),
	                             XMMS_IPC_SIGNAL_OUTPUT_CURRENTID);

	/* Signals are only emitted if the client has a pending question to it
	 * after the client recivies a signal, he must ask for it again */
	xmms_ipc_signal_register (XMMS_OBJECT (output),
	                          XMMS_IPC_SIGNAL_OUTPUT_PLAYTIME);


	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_START,
	                     XMMS_CMD_FUNC (start));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_STOP,
	                     XMMS_CMD_FUNC (stop));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_PAUSE,
	                     XMMS_CMD_FUNC (pause));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_DECODER_KILL,
	                     XMMS_CMD_FUNC (xform_kill));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_CPLAYTIME,
	                     XMMS_CMD_FUNC (playtime));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_SEEKMS,
	                     XMMS_CMD_FUNC (seekms));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_SEEKMS_REL,
	                     XMMS_CMD_FUNC (seekms_rel));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_SEEKSAMPLES,
	                     XMMS_CMD_FUNC (seeksamples));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_SEEKSAMPLES_REL,
	                     XMMS_CMD_FUNC (seeksamples_rel));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_OUTPUT_STATUS,
	                     XMMS_CMD_FUNC (output_status));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_CURRENTID,
	                     XMMS_CMD_FUNC (currentid));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_VOLUME_SET,
	                     XMMS_CMD_FUNC (volume_set));
	xmms_object_cmd_add (XMMS_OBJECT (output),
	                     XMMS_IPC_CMD_VOLUME_GET,
	                     XMMS_CMD_FUNC (volume_get));

	output->status = XMMS_PLAYBACK_STATUS_STOP;

	if (plugin) {
		if (!set_plugin (output, plugin)) {
			xmms_log_error ("Could not initialize output plugin");
		}
	} else {
		xmms_log_error ("initalized output without a plugin, please fix!");
	}



	return output;
}

/**
 * Flush the buffers in soundcard.
 */
void
xmms_output_flush (xmms_output_t *output)
{
	g_return_if_fail (output);

	xmms_output_plugin_method_flush (output->plugin, output);
}

/**
 * @internal
 */
static gboolean
xmms_output_format_set (xmms_output_t *output, xmms_stream_type_t *fmt)
{
	g_return_val_if_fail (output, FALSE);
	g_return_val_if_fail (fmt, FALSE);

	XMMS_DBG ("Setting format!");

	if (!xmms_output_plugin_format_set_always (output->plugin)) {
		gboolean ret;

		if (output->format && xmms_stream_type_match (output->format, fmt)) {
			XMMS_DBG ("audio formats are equal, not updating");
			return TRUE;
		}

		ret = xmms_output_plugin_method_format_set (output->plugin, output, fmt);
		if (ret) {
			xmms_object_unref (output->format);
			xmms_object_ref (fmt);
			output->format = fmt;
		}
		return ret;
	} else {
		if (output->format && !xmms_stream_type_match (output->format, fmt)) {
			xmms_object_unref (output->format);
			xmms_object_ref (fmt);
			output->format = fmt;
		}
		if (!output->format) {
			xmms_object_unref (output->format);
			xmms_object_ref (fmt);
			output->format = fmt;
		}
		return xmms_output_plugin_method_format_set (output->plugin, output, output->format);
	}
}


static gboolean
set_plugin (xmms_output_t *output, xmms_output_plugin_t *plugin)
{
	gboolean ret;

	g_assert (output);
	g_assert (plugin);

	output->monitor_volume_running = FALSE;
	if (output->monitor_volume_thread) {
		g_thread_join (output->monitor_volume_thread);
		output->monitor_volume_thread = NULL;
	}

	if (output->plugin) {
		xmms_output_plugin_method_destroy (output->plugin, output);
		output->plugin = NULL;
	}

	/* output->plugin needs to be set before we can call the
	 * NEW method
	 */
	output->plugin = plugin;
	ret = xmms_output_plugin_method_new (output->plugin, output);

	if (!ret) {
		output->plugin = NULL;
	} else if (!output->monitor_volume_thread) {
		output->monitor_volume_running = TRUE;
		output->monitor_volume_thread = g_thread_create (xmms_output_monitor_volume_thread,
		                                                 output, TRUE, NULL);
	}

	return ret;
}

static gint
xmms_volume_map_lookup (xmms_volume_map_t *vl, const gchar *name)
{
	gint i;

	for (i = 0; i < vl->num_channels; i++) {
		if (!strcmp (vl->names[i], name)) {
			return i;
		}
	}

	return -1;
}

/* returns TRUE when both hashes are equal, else FALSE */
static gboolean
xmms_volume_map_equal (xmms_volume_map_t *a, xmms_volume_map_t *b)
{
	guint i;

	g_assert (a);
	g_assert (b);

	if (a->num_channels != b->num_channels) {
		return FALSE;
	}

	for (i = 0; i < a->num_channels; i++) {
		gint j;

		j = xmms_volume_map_lookup (b, a->names[i]);
		if (j == -1 || b->values[j] != a->values[i]) {
			return FALSE;
		}
	}

	return TRUE;
}

static void
xmms_volume_map_init (xmms_volume_map_t *vl)
{
	vl->status = FALSE;
	vl->num_channels = 0;
	vl->names = NULL;
	vl->values = NULL;
}

static void
xmms_volume_map_free (xmms_volume_map_t *vl)
{
	g_free (vl->names);
	g_free (vl->values);

	/* don't free vl here, its always allocated on the stack */
}

static void
xmms_volume_map_copy (xmms_volume_map_t *src, xmms_volume_map_t *dst)
{
	dst->num_channels = src->num_channels;
	dst->status = src->status;

	if (!src->status) {
		g_free (dst->names);
		dst->names = NULL;

		g_free (dst->values);
		dst->values = NULL;

		return;
	}

	dst->names = g_renew (const gchar *, dst->names, src->num_channels);
	dst->values = g_renew (guint, dst->values, src->num_channels);

	memcpy (dst->names, src->names, src->num_channels * sizeof (gchar *));
	memcpy (dst->values, src->values, src->num_channels * sizeof (guint));
}

static GTree *
xmms_volume_map_to_dict (xmms_volume_map_t *vl)
{
	GTree *ret;
	gint i;

	ret = g_tree_new_full ((GCompareDataFunc) strcmp, NULL,
	                       NULL, (GDestroyNotify) xmmsv_unref);
	if (!ret) {
		return NULL;
	}

	for (i = 0; i < vl->num_channels; i++) {
		xmmsv_t *val;

		val = xmmsv_new_int (vl->values[i]);
		g_tree_replace (ret, (gpointer) vl->names[i], val);
	}

	return ret;
}

static gpointer
xmms_output_monitor_volume_thread (gpointer data)
{
	GTree *dict;
	xmms_output_t *output = data;
	xmms_volume_map_t old, cur;

	if (!xmms_output_plugin_method_volume_get_available (output->plugin)) {
		return NULL;
	}

	xmms_volume_map_init (&old);
	xmms_volume_map_init (&cur);

	while (output->monitor_volume_running) {
		cur.num_channels = 0;
		cur.status = xmms_output_plugin_method_volume_get (output->plugin,
		                                                   output, NULL, NULL,
		                                                   &cur.num_channels);

		if (cur.status) {
			/* check for sane values */
			if (cur.num_channels < 1 ||
			    cur.num_channels > VOLUME_MAX_CHANNELS) {
				cur.status = FALSE;
			} else {
				cur.names = g_renew (const gchar *, cur.names,
				                     cur.num_channels);
				cur.values = g_renew (guint, cur.values, cur.num_channels);
			}
		}

		if (cur.status) {
			cur.status =
				xmms_output_plugin_method_volume_get (output->plugin,
				                                      output, cur.names,
				                                      cur.values,
				                                      &cur.num_channels);
		}

		/* we failed at getting volume for one of the two maps or
		 * we succeeded both times and they differ -> changed
		 */
		if ((cur.status ^ old.status) ||
		    (cur.status && old.status &&
		     !xmms_volume_map_equal (&old, &cur))) {
			/* emit the broadcast */
			if (cur.status) {
				dict = xmms_volume_map_to_dict (&cur);
				xmms_object_emit_f (XMMS_OBJECT (output),
				                    XMMS_IPC_SIGNAL_OUTPUT_VOLUME_CHANGED,
				                    XMMSV_TYPE_DICT, dict);
				g_tree_destroy (dict);
			} else {
				/** @todo When bug 691 is solved, emit an error here */
				xmms_object_emit_f (XMMS_OBJECT (output),
				                    XMMS_IPC_SIGNAL_OUTPUT_VOLUME_CHANGED,
				                    XMMSV_TYPE_NONE);
			}
		}

		xmms_volume_map_copy (&cur, &old);

		g_usleep (G_USEC_PER_SEC);
	}

	xmms_volume_map_free (&old);
	xmms_volume_map_free (&cur);

	return NULL;
}

/** @} */

#include <gst/gst.h>

#include "local-player.h"

#define REPORT_MILLISECONDS 250

struct LocalPlayer {
	GstElement *playbin;
	LocalPlayerReport report;
	gpointer data;
	char *id;
	int state;
	guint report_source;
};


static double read_seconds(LocalPlayer *player, gboolean is_duration) {
	gint64 value = 0;
	gboolean is_known = is_duration ? gst_element_query_duration(player->playbin, GST_FORMAT_TIME, &value)
		: gst_element_query_position(player->playbin, GST_FORMAT_TIME, &value);
	return is_known ? (double)value / GST_SECOND : 0;
}


static void send_report(LocalPlayer *player) {
	if (!player->id)
		return;
	PlayerReport report = {player->id, player->state, read_seconds(player, FALSE), read_seconds(player, TRUE), FALSE};
	player->report(&report, player->data);
}


static gboolean report_time(gpointer data) {
	send_report(data);
	return G_SOURCE_CONTINUE;
}


static void set_state(LocalPlayer *player, int state) {
	player->state = state;
	gboolean is_playing = state == PLAYER_PLAYING;
	if (is_playing && !player->report_source)
		player->report_source = g_timeout_add(REPORT_MILLISECONDS, report_time, player);
	else if (!is_playing && player->report_source)
		g_clear_handle_id(&player->report_source, g_source_remove);
	send_report(player);
}


static gboolean watch_bus(GstBus *bus, GstMessage *message, gpointer data) {
	LocalPlayer *player = data;
	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_EOS:
		set_state(player, PLAYER_ENDED);
		break;
	case GST_MESSAGE_ERROR: {
		g_autoptr(GError) error = NULL;
		gst_message_parse_error(message, &error, NULL);
		g_warning("A local song could not play: %s", error->message);
		set_state(player, PLAYER_ENDED);
		break;
	}
	case GST_MESSAGE_STATE_CHANGED: {
		GstState state;
		if (GST_MESSAGE_SRC(message) != GST_OBJECT(player->playbin))
			break;
		gst_message_parse_state_changed(message, NULL, &state, NULL);
		if (state == GST_STATE_PLAYING)
			set_state(player, PLAYER_PLAYING);
		else if (state == GST_STATE_PAUSED && player->state == PLAYER_PLAYING)
			set_state(player, PLAYER_PAUSED);
		break;
	}
	default:
		break;
	}
	return G_SOURCE_CONTINUE;
}


static gboolean is_gstreamer_ready, is_gstreamer_starting;
/* A song asked to play while GStreamer was still starting, played once it is ready */
static LocalPlayer *waiting_player;
static char *waiting_id;
static int waiting_volume;


static void start_gstreamer(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	gst_init(NULL, NULL);
	g_task_return_boolean(task, TRUE);
}


static void finish_gstreamer(GObject *source, GAsyncResult *result, gpointer data) {
	is_gstreamer_ready = TRUE;
	if (!waiting_player)
		return;
	g_autofree char *id = g_steal_pointer(&waiting_id);
	LocalPlayer *player = g_steal_pointer(&waiting_player);
	play_local_song(player, id, waiting_volume);
}


/* GStreamer can take seconds to look through its plugins after an update, so it starts on a worker thread once the
 * window shows, and the main thread never waits for it */
void prepare_local_player(void) {
	if (is_gstreamer_ready || is_gstreamer_starting)
		return;
	is_gstreamer_starting = TRUE;
	g_autoptr(GTask) task = g_task_new(NULL, NULL, finish_gstreamer, NULL);
	g_task_run_in_thread(task, start_gstreamer);
}


/* The player itself is made when the first local song plays, so it never holds up the window opening */
LocalPlayer *create_local_player(LocalPlayerReport report, gpointer data) {
	LocalPlayer *player = g_new0(LocalPlayer, 1);
	player->report = report;
	player->data = data;
	return player;
}


static void prepare_playbin(LocalPlayer *player) {
	if (player->playbin)
		return;
	player->playbin = gst_element_factory_make("playbin", NULL);
	/* Only the sound is wanted, so a file with a picture track never opens a video window */
	GstElement *fake = gst_element_factory_make("fakesink", NULL);
	g_object_set(player->playbin, "video-sink", fake, NULL);
	g_autoptr(GstBus) bus = gst_element_get_bus(player->playbin);
	gst_bus_add_watch(bus, watch_bus, player);
}


void play_local_song(LocalPlayer *player, const char *id, int volume) {
	if (!is_gstreamer_ready) {
		waiting_player = player;
		g_set_str(&waiting_id, id);
		waiting_volume = volume;
		prepare_local_player();
		return;
	}
	prepare_playbin(player);
	gst_element_set_state(player->playbin, GST_STATE_NULL);
	g_free(player->id);
	player->id = g_strdup(id);
	g_object_set(player->playbin, "uri", id, NULL);
	set_local_volume(player, volume);
	player->state = PLAYER_BUFFERING;
	gst_element_set_state(player->playbin, GST_STATE_PLAYING);
}


void pause_local_song(LocalPlayer *player) {
	if (player->playbin)
		gst_element_set_state(player->playbin, GST_STATE_PAUSED);
}


void resume_local_song(LocalPlayer *player) {
	if (player->playbin)
		gst_element_set_state(player->playbin, GST_STATE_PLAYING);
}


void seek_local_song(LocalPlayer *player, double seconds) {
	if (!player->playbin)
		return;
	gst_element_seek_simple(player->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
		(gint64)(seconds * GST_SECOND));
	send_report(player);
}


void set_local_volume(LocalPlayer *player, int volume) {
	if (player->playbin)
		g_object_set(player->playbin, "volume", volume / 100.0, NULL);
}


void stop_local_song(LocalPlayer *player) {
	if (waiting_player == player)
		g_clear_pointer(&waiting_id, g_free);
	waiting_player = waiting_player == player ? NULL : waiting_player;
	if (player->playbin)
		gst_element_set_state(player->playbin, GST_STATE_NULL);
	g_clear_pointer(&player->id, g_free);
	g_clear_handle_id(&player->report_source, g_source_remove);
	player->state = PLAYER_ENDED;
}

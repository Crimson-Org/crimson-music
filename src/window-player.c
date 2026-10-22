#include "common/log.h"
#include "info-dialog.h"
#include "last-song.h"
#include "local-library.h"
#include "local-player.h"
#include "lastfm.h"
#include "settings.h"
#include "song-list.h"
#include "song-info.h"
#include "window-private.h"

/* Previous restarts the song instead once it has played this long, as in other players */
#define RESTART_SECONDS 3
/* Shift and an arrow jump this far through the song */
#define JUMP_SECONDS 10
/* Play and pause answer at most this often and not while loading, since a burst of presses queues up the page's
 * player */
#define TOGGLE_MICROSECONDS (250 * 1000)
/* The queue shows this many of the songs coming up */
#define UPCOMING_LIMIT 100
/* A step of the playing time longer than this was a seek, not listening */
#define HEARD_STEP_MAXIMUM 3
#define SCROBBLE_LENGTH_MINIMUM 30
#define SCROBBLE_HEARD_MAXIMUM 240

static void play_queue_song(Window *window);


void show_upcoming(Window *window) {
	g_autoptr(GPtrArray) upcoming = list_upcoming(window->queue, UPCOMING_LIMIT);
	guint queued = 0;
	while (queued < upcoming->len && is_upcoming_queued(window->queue, queued + 1))
		queued++;
	show_player_upcoming(window->bar, upcoming, queued);
	if (!get_settings()->restores_last_song)
		return;
	save_last_song(get_queue_song(window->queue), upcoming, queued, window->queue_page_tag, window->queue_playlist_id);
}


void move_queued_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	int from = 0, to = 0;
	g_variant_get(parameter, "(ii)", &from, &to);
	move_upcoming(window->queue, (guint)from, (guint)to);
	show_upcoming(window);
}


void remove_queued_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	remove_upcoming(window->queue, (guint)g_variant_get_int32(parameter));
	show_upcoming(window);
}


void jump_queue(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (step_queue(window->queue, g_variant_get_int32(parameter)))
		play_queue_song(window);
}


/* Local songs play through a player of their own, and everything else through the YouTube page */
static gboolean is_local_playing(Window *window) {
	return is_local_song(window->current_video);
}


static void pause_playing(Window *window) {
	if (is_local_playing(window))
		pause_local_song(window->local_player);
	else
		pause_video(window->engine);
}


static void resume_playing(Window *window) {
	if (is_local_playing(window))
		resume_local_song(window->local_player);
	else
		resume_video(window->engine);
}


static void seek_playing(Window *window, double seconds) {
	if (is_local_playing(window))
		seek_local_song(window->local_player, seconds);
	else
		seek_video(window->engine, seconds);
}


/* Shows the play button as playing, paused or loading, and remembers loading so play and pause wait for it */
static void show_playing(Window *window, gboolean is_playing, gboolean is_waiting) {
	window->is_waiting = is_waiting;
	show_player_playing(window->bar, is_playing, is_waiting);
}


/* Shows the queue's song in the player bar, playing or paused */
static void show_queue_song(Window *window, CrimsonSong *song, gboolean is_playing) {
	g_set_str(&window->current_video, song->id);
	window->time = 0;
	window->has_started = FALSE;
	window->heard_seconds = 0;
	window->is_scrobbled = FALSE;
	set_player_seekable(window->bar, FALSE);
	set_playing_video(song->id);
	show_player_song(window->bar, song);
	set_mpris_song(window->mpris, song);
	ask_for_lyrics(window, song);
	ask_for_comments(window, song);
	show_player_liked(window->bar, is_video_liked(song->id));
	show_player_disliked(window->bar, is_video_disliked(song->id));
	set_player_likable(window->bar, !is_local_song(song->id));
	show_playing_love(window);
	g_autoptr(GPtrArray) chapters = get_song_chapters(song->id);
	show_player_chapters(window->bar, chapters);
	gather_song_info_now(song);
	show_upcoming(window);
	show_playing(window, is_playing, is_playing);
	gtk_revealer_set_reveal_child(GTK_REVEALER(window->player_revealer), TRUE);
	update_top_title(window);
}


static void play_queue_song(Window *window) {
	CrimsonSong *song = get_queue_song(window->queue);
	if (!song)
		return;
	/* The restored song is already on show, so only its button changes and nothing slides in again */
	if (window->is_restored && g_strcmp0(window->current_video, song->id) == 0)
		show_playing(window, TRUE, TRUE);
	else
		show_queue_song(window, song, TRUE);
	window->is_restored = FALSE;
	if (is_local_song(song->id)) {
		pause_video(window->engine);
		play_local_song(window->local_player, song->id, window->state.volume);
	} else {
		stop_local_song(window->local_player);
		play_video(window->engine, song->id, window->state.volume, get_settings()->resumes_songs);
	}
}


/* The song the listener had up comes back paused, with the songs that were up next after it in the same order, so
 * Play carries on as if the app had never closed */
void restore_last_song(Window *window) {
	if (!get_settings()->restores_last_song)
		return;
	LastSong *last = load_last_song();
	if (!last)
		return;
	g_autofree char *path = is_local_song(last->song->id) ? get_local_song_path(last->song->id) : NULL;
	if (path && !g_file_test(path, G_FILE_TEST_EXISTS)) {
		free_last_song(last);
		return;
	}
	g_autoptr(GListStore) songs = g_list_store_new(CRIMSON_TYPE_SONG);
	g_list_store_append(songs, last->song);
	for (guint index = 0; index < last->upcoming->len; index++)
		g_list_store_append(songs, g_ptr_array_index(last->upcoming, index));
	g_set_str(&window->queue_page_tag, last->page_tag);
	g_set_str(&window->queue_playlist_id, last->playlist_id);
	fill_queue(window->queue, G_LIST_MODEL(songs), 0, FALSE);
	mark_upcoming_queued(window->queue, last->queued);
	window->is_restored = TRUE;
	show_queue_song(window, last->song, FALSE);
	free_last_song(last);
}


/* Shuffling from a page turns shuffle on, while a plain click keeps the listener's choice */
void play_songs(GListModel *songs, guint start, gboolean is_shuffled, gpointer data) {
	Window *window = data;
	if (is_shuffled)
		g_action_change_state(G_ACTION(window->shuffle_action), g_variant_new_boolean(TRUE));
	AdwNavigationPage *page = adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(window->navigation));
	g_set_str(&window->queue_page_tag, adw_navigation_page_get_tag(page));
	g_set_str(&window->queue_playlist_id, window->shown ? window->shown->id : NULL);
	fill_queue(window->queue, songs, start, window->state.is_shuffled);
	play_queue_song(window);
}


/* Clicking the song already in the player leaves it playing, or carries it on when paused, rather than starting it
 * over */
void choose_song(GListModel *songs, guint position, gpointer data) {
	Window *window = data;
	g_autoptr(CrimsonSong) song = g_list_model_get_item(songs, position);
	if (!song || g_strcmp0(song->id, window->current_video) != 0) {
		play_songs(songs, position, FALSE, window);
		return;
	}
	if (!window->is_playing)
		toggle_playing(NULL, NULL, window);
}


void toggle_playing(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	static gint64 last_toggle;
	gint64 now = g_get_monotonic_time();
	if (now - last_toggle < TOGGLE_MICROSECONDS || window->is_waiting)
		return;
	last_toggle = now;
	if (window->is_restored)
		play_queue_song(window);
	else if (!window->current_video && window->shown)
		play_songs(G_LIST_MODEL(window->shown->songs), 0, FALSE, window);
	else if (window->is_playing)
		pause_playing(window);
	else
		resume_playing(window);
}


void play_next(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (step_queue(window->queue, 1))
		play_queue_song(window);
}


void play_previous(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->time > RESTART_SECONDS || !step_queue(window->queue, -1))
		seek_playing(window, 0);
	else
		play_queue_song(window);
}


void change_shuffle(GSimpleAction *action, GVariant *state, gpointer data) {
	Window *window = data;
	window->state.is_shuffled = g_variant_get_boolean(state);
	g_simple_action_set_state(action, state);
	shuffle_queue(window->queue, window->state.is_shuffled);
	show_upcoming(window);
}


void seek(double seconds, gpointer data) {
	Window *window = data;
	if (window->has_started)
		seek_playing(window, seconds);
}


void change_volume(int volume, gpointer data) {
	Window *window = data;
	window->state.volume = volume;
	set_video_volume(window->engine, volume);
	set_local_volume(window->local_player, volume);
}


/* Only time that really played counts, so seeking ahead never makes a listen, and a song counts once it played
 * for half its length or four minutes, if it is longer than half a minute, as Last.fm defines a listen */
static void count_heard_time(Window *window, const PlayerReport *report) {
	double step = report->time - window->last_time;
	window->last_time = report->time;
	if (report->state != PLAYER_PLAYING || step <= 0 || step > HEARD_STEP_MAXIMUM)
		return;
	window->heard_seconds += step;
	gboolean is_long_enough = report->duration > SCROBBLE_LENGTH_MINIMUM;
	double needed = MIN(report->duration / 2, SCROBBLE_HEARD_MAXIMUM);
	CrimsonSong *song = get_queue_song(window->queue);
	if (!window->is_scrobbled && is_long_enough && window->heard_seconds >= needed && song) {
		window->is_scrobbled = TRUE;
		scrobble_song(song, window->started_at, report->duration);
	}
}


void receive_player(const PlayerReport *report, gpointer data) {
	Window *window = data;
	if (g_strcmp0(report->video_id, window->current_video) != 0)
		return;
	if (report->state == PLAYER_PLAYING && !window->has_started) {
		static gboolean is_first = TRUE;
		if (is_first)
			note_startup_moment("First song playing");
		is_first = FALSE;
		window->has_started = TRUE;
		window->last_time = report->time;
		window->started_at = g_get_real_time() / G_USEC_PER_SEC;
		set_player_seekable(window->bar, TRUE);
		CrimsonSong *song = get_queue_song(window->queue);
		if (song)
			send_now_playing(song, report->duration);
	}
	count_heard_time(window, report);
	window->time = report->time;
	window->is_playing = report->state == PLAYER_PLAYING || report->state == PLAYER_BUFFERING;
	gboolean is_waiting = report->state != PLAYER_PLAYING && report->state != PLAYER_PAUSED
		&& report->state != PLAYER_ENDED;
	show_playing(window, window->is_playing, is_waiting);
	set_tray_playing(window->tray, window->is_playing);
	set_mpris_playing(window->mpris, window->is_playing);
	set_mpris_position(window->mpris, report->time);
	show_player_time(window->bar, report->time, report->duration);
	follow_playing_lyrics(window);
	follow_music_video(window, report);
	if (report->state == PLAYER_ENDED)
		play_next(NULL, NULL, window);
}


/* New information on the playing song may bring its chapters */
void show_playing_love(Window *window) {
	CrimsonSong *song = get_queue_song(window->queue);
	show_player_loved(window->bar, get_lastfm_user() != NULL && song, song && is_song_loved(song));
}


void notice_song_info(const char *video_id, gpointer data) {
	Window *window = data;
	update_info_dialog(video_id);
	if (is_local_song(video_id))
		update_local_song_info(video_id);
	update_dynamic_playlists(window);
	if (g_strcmp0(video_id, window->current_video) == 0) {
		g_autoptr(GPtrArray) chapters = get_song_chapters(video_id);
		show_player_chapters(window->bar, chapters);
	}
}


static void jump(Window *window, double seconds) {
	if (window->current_video && window->has_started)
		seek_playing(window, MAX(0, window->time + seconds));
}


void jump_back(GSimpleAction *action, GVariant *parameter, gpointer data) {
	jump(data, -JUMP_SECONDS);
}


void jump_forward(GSimpleAction *action, GVariant *parameter, gpointer data) {
	jump(data, JUMP_SECONDS);
}

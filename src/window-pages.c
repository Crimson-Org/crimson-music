#include <string.h>

#include "../lang/en.h"
#include "lyrics-page.h"
#include "artwork.h"
#include "comments-page.h"
#include "common/startup.h"
#include "collage.h"
#include "discover.h"
#include "dynamic.h"
#include "info-dialog.h"
#include "lastfm.h"
#include "local-library.h"
#include "motion.h"
#include "settings.h"
#include "song-info.h"
#include "song-list.h"
#include "window-private.h"

#define LASTFM_MUSIC_ADDRESS "https://www.last.fm/music/"
/* A song or video plays on its own page here, the same page for both kinds */
#define SONG_PAGE_ADDRESS "https://music.youtube.com/watch?v="
#define LASTFM_ALBUM_SEARCH "https://www.last.fm/search/albums?q="
/* Pages of similar songs or artists are tagged by the song or artist they start from */
#define SEARCH_UNROLL_MS 420
#define SIMILAR_SONGS_TAG "similar-songs:"
#define SONG_VIEW_SLIDE_MS 350
/* The comments panel is this wide at the right of the window */
#define COMMENTS_WIDTH 360
/* Lyrics and video take this share of the middle each while both are open */
#define SHARED_SHARE 0.5
#define SIMILAR_ARTISTS_TAG "similar-artists:"
#define LASTFM_LIBRARY_PATH "/library/music/"

#define ARTIST_PREFIX "UC"
/* Cards of songs in the listener's playlists carry the playlist and song ids after this, split by a newline */
#define LIBRARY_PREFIX "library:"
/* Cards of songs played lately carry the song id after this */
#define HISTORY_PREFIX "history:"
/* Home shows this many of the songs played lately, and the rest are on the page of them all */
#define HISTORY_CARD_LIMIT 20
/* The account picture is asked for at this size, sharp enough for the Home banner */
#define ACCOUNT_PHOTO_SIZE "=s512"
#define LIBRARY_RESULT_LIMIT 30
/* The mouse's side buttons as GTK numbers them */
#define BACK_BUTTON 8
#define FORWARD_BUTTON 9
/* Liked Music and Episodes for Later belong to YouTube Music itself and can be neither renamed nor deleted */
/* How many of an artist page's songs are songs rather than videos, so more songs go before the videos */
#define SONG_COUNT_KEY "song-count"
#define LATER_PLAYLIST "SE"


static gboolean is_own_playlist(CrimsonPlaylist *playlist) {
	return g_strcmp0(playlist->id, LIKED_PLAYLIST) != 0 && g_strcmp0(playlist->id, LATER_PLAYLIST) != 0;
}


static SongPage *find_song_page(Window *window, const char *tag) {
	AdwNavigationPage *page = adw_navigation_view_find_page(ADW_NAVIGATION_VIEW(window->navigation), tag);
	return page ? g_object_get_data(G_OBJECT(adw_navigation_page_get_child(page)), "song-page") : NULL;
}


/* The search page slides in at once with a spinner, since results follow every keystroke */
static gboolean unroll_search(gpointer data) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(data), TRUE);
	g_object_unref(data);
	return G_SOURCE_REMOVE;
}


/* A page of songs alone scrolls them as a list that only builds the rows on screen, like a playlist */
static SongPage *push_song_page(Window *window, const char *tag, const char *title, gboolean is_songs_only) {
	SongPage *page = create_song_page(&SONG_PAGE_EVENTS, window, is_songs_only);
	set_song_page_heading(page, NULL, NULL, NULL, FALSE);
	update_song_page(page, FALSE);
	GtkWidget *widget = get_song_page_widget(page);
	/* Search unrolls downward from the search field above it */
	if (g_str_equal(tag, SEARCH_TAG)) {
		GtkWidget *revealer = gtk_revealer_new();
		gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
		gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), SEARCH_UNROLL_MS);
		gtk_widget_set_vexpand(revealer, TRUE);
		gtk_widget_set_vexpand(widget, TRUE);
		gtk_revealer_set_child(GTK_REVEALER(revealer), widget);
		g_object_set_data(G_OBJECT(revealer), "song-page", page);
		run_after_frame(revealer, unroll_search, g_object_ref(revealer));
		widget = revealer;
	}
	AdwNavigationPage *navigation_page = adw_navigation_page_new_with_tag(widget, title, tag);
	/* Search results drop down from the search field above them, where other pages slide in from the side */
	gboolean is_search = g_str_equal(tag, SEARCH_TAG);
	AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(window->navigation);
	adw_navigation_view_set_animate_transitions(navigation, !is_search);
	adw_navigation_view_push(navigation, navigation_page);
	adw_navigation_view_set_animate_transitions(navigation, TRUE);
	if (is_search)
		set_song_page_waits_for_cards(page, TRUE);
	return page;
}


/* Shows the page with the tag again when it is already open, or opens a new one */
static SongPage *open_song_page(Window *window, const char *tag, const char *title, gboolean is_songs_only,
	gboolean *is_new) {
	AdwNavigationPage *open = adw_navigation_view_find_page(ADW_NAVIGATION_VIEW(window->navigation), tag);
	*is_new = open == NULL;
	if (!open)
		return push_song_page(window, tag, title, is_songs_only);
	adw_navigation_view_pop_to_page(ADW_NAVIGATION_VIEW(window->navigation), open);
	return find_song_page(window, tag);
}


static void show_waiting(Window *window, const char *id) {
	g_set_str(&window->waiting_card_id, id);
	gtk_widget_set_visible(window->top_spinner, id != NULL);
}


/* A page still waiting when its answer failed shows as empty rather than loading for ever */
void stop_waiting(Window *window) {
	SongPage *page = window->waiting_card_id ? find_song_page(window, window->waiting_card_id) : NULL;
	if (page && !get_song_page_songs(page)) {
		set_song_page_empty(page, "dialog-question-symbolic", TEXT_NOT_FOUND, TEXT_NOT_FOUND_DETAIL);
		set_song_page_waits_for_cards(page, FALSE);
		update_song_page(page, TRUE);
	}
	show_waiting(window, NULL);
	window->is_going_forward = FALSE;
}


/* An artist or album already open comes forward again, and a new one slides in only once its content is here,
 * with a spinner in the top bar until then */
/* Finds the playlist and the song a search result from the listener's playlists stands for */
static CrimsonPlaylist *find_library_song(Window *window, const char *id, guint *position) {
	g_auto(GStrv) parts = g_strsplit(id + strlen(LIBRARY_PREFIX), "\n", 2);
	CrimsonPlaylist *playlist = parts[1] ? find_playlist(window, parts[0]) : NULL;
	for (guint index = 0; playlist && index < g_list_model_get_n_items(G_LIST_MODEL(playlist->songs)); index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(playlist->songs), index);
		if (g_str_equal(song->id, parts[1])) {
			*position = index;
			return playlist;
		}
	}
	return NULL;
}


/* Plays the song on from its place in its playlist while the search stays open, and the title in the player
 * bar still leads to that playlist */
static void play_library_song(Window *window, const char *id) {
	guint position = 0;
	CrimsonPlaylist *playlist = find_library_song(window, id, &position);
	if (!playlist)
		return;
	play_songs(G_LIST_MODEL(playlist->songs), position, FALSE, window);
	g_set_str(&window->queue_page_tag, PLAYLIST_TAG);
	g_set_str(&window->queue_playlist_id, playlist->id);
}


/* Going to the playlist from a search result leaves a way back to the search */
static void go_to_library_playlist(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	guint position = 0;
	CrimsonPlaylist *playlist = find_library_song(window, g_variant_get_string(parameter, NULL), &position);
	if (!playlist)
		return;
	select_playlist_row(window, playlist->id);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
	window->is_back_to_search = TRUE;
	update_back_button(window);
}


/* The newest play of the song, as a new reference, or NULL when Home no longer lists it */
static CrimsonSong *find_history_song(Window *window, const char *id, guint *position) {
	GListModel *songs = window->history ? G_LIST_MODEL(window->history) : NULL;
	for (guint index = 0; songs && index < g_list_model_get_n_items(songs); index++) {
		CrimsonSong *song = g_list_model_get_item(songs, index);
		if (g_str_equal(song->id, id + strlen(HISTORY_PREFIX))) {
			*position = index;
			return song;
		}
		g_object_unref(song);
	}
	return NULL;
}


/* The history plays on from the song, and the title in the player bar leads to the page of every played song */
static void play_history_song(Window *window, const char *id) {
	guint position = 0;
	g_autoptr(CrimsonSong) song = find_history_song(window, id, &position);
	if (!song)
		return;
	play_songs(G_LIST_MODEL(window->history), position, FALSE, window);
	g_set_str(&window->queue_page_tag, window->history_id);
	g_clear_pointer(&window->queue_playlist_id, g_free);
}


static void open_home(Window *window);
static void open_discover_mix(Window *window, const char *id);


void open_card(const char *id, gpointer data) {
	Window *window = data;
	if (!id || !*id || g_strcmp0(id, window->waiting_card_id) == 0)
		return;
	if (g_str_has_prefix(id, LIBRARY_PREFIX)) {
		play_library_song(window, id);
		return;
	}
	if (g_str_has_prefix(id, HISTORY_PREFIX)) {
		play_history_song(window, id);
		return;
	}
	AdwNavigationPage *open = adw_navigation_view_find_page(ADW_NAVIGATION_VIEW(window->navigation), id);
	if (open) {
		adw_navigation_view_pop_to_page(ADW_NAVIGATION_VIEW(window->navigation), open);
		return;
	}
	if (g_str_equal(id, HOME_TAG)) {
		open_home(window);
		return;
	}
	if (g_str_has_prefix(id, DISCOVER_MIX_PREFIX)) {
		open_discover_mix(window, id);
		return;
	}
	/* The page slides in at once with placeholders, and its heading, cards and songs follow when they arrive */
	show_waiting(window, id);
	gboolean is_artist = g_str_has_prefix(id, ARTIST_PREFIX);
	SongPage *page = push_song_page(window, id, "", !is_artist);
	set_song_page_heading(page, "", "", NULL, is_artist);
	set_song_page_waits_for_cards(page, is_artist);
	keep_song_page_about(page);
	if (is_artist)
		request_artist(window->engine, id);
	else
		request_album(window->engine, id);
}


/* Every word of the search must be in the song's title, artist or album */
static gboolean is_matching_song(CrimsonSong *song, char **words) {
	g_autofree char *text = g_strjoin(" ", song->title, song->artist, song->album, NULL);
	g_autofree char *folded = g_utf8_casefold(text, -1);
	for (guint index = 0; words[index]; index++) {
		if (*words[index] && !strstr(folded, words[index]))
			return FALSE;
	}
	return TRUE;
}


/* Songs from the listener's own playlists, read from the cache, so they show the moment the search is typed */
static GPtrArray *find_library_songs(Window *window, const char *query) {
	g_autofree char *folded = g_utf8_casefold(query, -1);
	g_auto(GStrv) words = g_strsplit(folded, " ", -1);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GPtrArray *cards = g_ptr_array_new_with_free_func(free_card);
	GListModel *playlists = G_LIST_MODEL(window->playlists);
	for (guint list = 0; list < g_list_model_get_n_items(playlists) && cards->len < LIBRARY_RESULT_LIMIT; list++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(playlists, list);
		if (!playlist->has_songs)
			load_cached_songs(playlist);
		GListModel *songs = G_LIST_MODEL(playlist->songs);
		for (guint index = 0; index < g_list_model_get_n_items(songs) && cards->len < LIBRARY_RESULT_LIMIT; index++) {
			g_autoptr(CrimsonSong) song = g_list_model_get_item(songs, index);
			if (g_hash_table_contains(seen, song->id) || !is_matching_song(song, words))
				continue;
			g_hash_table_add(seen, song->id);
			Card *card = g_new0(Card, 1);
			card->id = g_strconcat(LIBRARY_PREFIX, playlist->id, "\n", song->id, NULL);
			card->title = g_strdup(song->title);
			card->subtitle = g_strdup_printf("%s · %s", song->artist, playlist->title);
			card->artwork = g_strdup(song->artwork);
			card->video_id = g_strdup(song->id);
			g_ptr_array_add(cards, card);
		}
	}
	return cards;
}


static void show_library_results(Window *window, SongPage *page, const char *query) {
	remove_song_page_cards(page, window->library_results);
	g_autoptr(GPtrArray) cards = find_library_songs(window, query);
	window->library_results = add_song_page_cards(page, TEXT_IN_YOUR_PLAYLISTS, cards, FALSE, TRUE);
	update_song_page(page, FALSE);
}


/* Matches from the listener's playlists show at once, while YouTube's own results follow below them */
void start_search(GtkSearchEntry *entry, Window *window) {
	close_lyrics(window);
	g_autofree char *query = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	if (!*query) {
		AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(window->navigation);
		adw_navigation_view_set_animate_transitions(navigation, FALSE);
		adw_navigation_view_pop_to_tag(navigation, PLAYLIST_TAG);
		adw_navigation_view_set_animate_transitions(navigation, TRUE);
		return;
	}
	gboolean is_new = FALSE;
	SongPage *page = open_song_page(window, SEARCH_TAG, TEXT_SEARCH, FALSE, &is_new);
	if (is_new) {
		window->library_results = NULL;
		g_ptr_array_set_size(window->online_results, 0);
	}
	show_library_results(window, page, query);
	request_search(window->engine, query, ++window->search_token);
}


static double read_number(JSCValue *message, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(message, name);
	return jsc_value_is_number(value) ? jsc_value_to_double(value) : 0;
}


static GListModel *create_song_store(JSCValue *message) {
	g_autoptr(JSCValue) array = jsc_value_object_get_property(message, "songs");
	g_autoptr(GPtrArray) songs = read_songs(array);
	GListStore *store = g_list_store_new(CRIMSON_TYPE_SONG);
	g_list_store_splice(store, 0, 0, songs->pdata, songs->len);
	return G_LIST_MODEL(store);
}


static GtkWidget *add_cards(SongPage *page, JSCValue *message, const char *name, const char *title,
	gboolean is_round) {
	g_autoptr(JSCValue) array = jsc_value_object_get_property(message, name);
	g_autoptr(GPtrArray) cards = read_cards(array);
	return add_song_page_cards(page, title, cards, is_round, FALSE);
}


static char *read_text(JSCValue *message, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(message, name);
	return jsc_value_is_string(value) ? jsc_value_to_string(value) : g_strdup("");
}


typedef struct {
	Window *window;
	char *tag, *title;
} HeadingWait;


static void show_artists_collage(const char *path, gpointer data) {
	HeadingWait *wait = data;
	SongPage *page = path ? find_song_page(wait->window, wait->tag) : NULL;
	if (page)
		set_song_page_heading(page, wait->title, NULL, path, FALSE);
	g_free(wait->tag);
	g_free(wait->title);
	g_free(wait);
}


/* A page of artists has no cover of its own, so the first few artists' pictures make one */
static void make_artists_collage(Window *window, const char *tag, GPtrArray *cards) {
	g_autoptr(GPtrArray) covers = g_ptr_array_new();
	for (guint index = 0; index < cards->len; index++) {
		Card *card = g_ptr_array_index(cards, index);
		if (card->artwork && *card->artwork)
			g_ptr_array_add(covers, card->artwork);
	}
	HeadingWait *wait = g_new0(HeadingWait, 1);
	wait->window = window;
	wait->tag = g_strdup(tag);
	SongPage *page = find_song_page(window, tag);
	wait->title = g_strdup(page ? g_object_get_data(G_OBJECT(get_song_page_widget(page)), "similar-title") : "");
	make_collage(covers, show_artists_collage, wait);
}


static void fill_similar(Window *window, JSCValue *message, gboolean is_artists) {
	g_autoptr(JSCValue) token = jsc_value_object_get_property(message, "token");
	SongPage *page = window->similar_tag ? find_song_page(window, window->similar_tag) : NULL;
	if (!page || jsc_value_to_int32(token) != window->similar_token)
		return;
	set_song_page_empty_title(page, is_artists ? TEXT_NO_SIMILAR_ARTISTS : TEXT_NO_SIMILAR_SONGS);
	if (is_artists) {
		g_autoptr(JSCValue) array = jsc_value_object_get_property(message, "artists");
		g_autoptr(GPtrArray) cards = read_cards(array);
		add_song_page_card_grid(page, cards, FALSE);
		g_autoptr(GListModel) none = G_LIST_MODEL(g_list_store_new(CRIMSON_TYPE_SONG));
		set_song_page_songs(page, none, FALSE, TRUE);
		make_artists_collage(window, window->similar_tag, cards);
	} else {
		g_autoptr(GListModel) songs = create_song_store(message);
		set_song_page_songs(page, songs, FALSE, TRUE);
	}
	update_song_page(page, TRUE);
}


static void fill_search(Window *window, JSCValue *message) {
	g_autoptr(JSCValue) token = jsc_value_object_get_property(message, "token");
	SongPage *page = find_song_page(window, SEARCH_TAG);
	if (!page || jsc_value_to_int32(token) != window->search_token)
		return;
	for (guint index = 0; index < window->online_results->len; index++)
		remove_song_page_cards(page, g_ptr_array_index(window->online_results, index));
	g_ptr_array_set_size(window->online_results, 0);
	const char *names[] = {"artists", "albums"};
	const char *titles[] = {TEXT_ARTISTS, TEXT_ALBUMS};
	for (guint index = 0; index < G_N_ELEMENTS(names); index++) {
		GtkWidget *section = add_cards(page, message, names[index], titles[index], FALSE);
		if (section)
			g_ptr_array_add(window->online_results, section);
	}
	g_autoptr(GListModel) songs = create_song_store(message);
	set_song_page_empty_title(page, TEXT_NO_RESULTS);
	set_song_page_songs(page, songs, FALSE, TRUE);
	update_song_page(page, TRUE);
}


/* Fills the whole page before it slides in, so it arrives complete, and only for the page still waited for */
/* The full lists behind an artist's Show all go after the songs and videos already shown */
static void fill_more(Window *window, JSCValue *message) {
	g_autofree char *id = read_text(message, "id");
	SongPage *page = find_song_page(window, id);
	GListStore *store = page ? G_LIST_STORE(get_song_page_songs(page)) : NULL;
	if (!store)
		return;
	g_autoptr(JSCValue) song_array = jsc_value_object_get_property(message, "songs");
	g_autoptr(JSCValue) video_array = jsc_value_object_get_property(message, "videos");
	g_autoptr(GPtrArray) songs = read_songs(song_array);
	g_autoptr(GPtrArray) videos = read_songs(video_array);
	guint count = (guint)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(store), SONG_COUNT_KEY));
	g_list_store_splice(store, count, 0, songs->pdata, songs->len);
	g_object_set_data(G_OBJECT(store), SONG_COUNT_KEY, GINT_TO_POINTER((int)(count + songs->len)));
	g_list_store_splice(store, g_list_model_get_n_items(G_LIST_MODEL(store)), 0, videos->pdata, videos->len);
}


typedef struct {
	Window *window;
	char *id;
} AboutWait;


/* Listener and play counts read better rounded, such as 1.2M */
static char *format_count(const char *digits) {
	double count = g_ascii_strtod(digits ? digits : "", NULL);
	if (count >= 1e6)
		return g_strdup_printf("%.1fM", count / 1e6);
	if (count >= 1e3)
		return g_strdup_printf("%.0fK", count / 1e3);
	return g_strdup_printf("%.0f", count);
}


/* The page may have been left before Last.fm answered, so it is looked up again by its id */
static void show_about(GHashTable *about, gpointer data) {
	AboutWait *wait = data;
	SongPage *page = about ? find_song_page(wait->window, wait->id) : NULL;
	if (page) {
		GPtrArray *facts = g_ptr_array_new_with_free_func(g_free);
		const char *listeners = g_hash_table_lookup(about, "listeners");
		const char *plays = g_hash_table_lookup(about, "plays");
		const char *tags = g_hash_table_lookup(about, "tags");
		if (listeners && *listeners && g_strcmp0(listeners, "0") != 0) {
			g_autofree char *count = format_count(listeners);
			g_ptr_array_add(facts, g_strdup_printf(TEXT_ABOUT_LISTENERS, count));
		}
		if (plays && *plays && g_strcmp0(plays, "0") != 0) {
			g_autofree char *count = format_count(plays);
			g_ptr_array_add(facts, g_strdup_printf(TEXT_ABOUT_PLAYS, count));
		}
		if (tags && *tags)
			g_ptr_array_add(facts, g_strdup(tags));
		g_ptr_array_add(facts, NULL);
		g_autofree char *line = g_strjoinv(" · ", (char **)facts->pdata);
		g_ptr_array_unref(facts);
		set_song_page_about(page, g_hash_table_lookup(about, "summary"), line);
	}
	g_free(wait->id);
	g_free(wait);
}


static void fetch_about(Window *window, const char *id, const char *artist, const char *album) {
	AboutWait *wait = g_new0(AboutWait, 1);
	wait->window = window;
	wait->id = g_strdup(id);
	fetch_lastfm_about(artist, album, show_about, wait);
}


/* The subtitle of the card Home shows with the id, or NULL when Home shows no such card, which works before Home
 * was opened too, such as for a pinned mix */
static const char *find_home_subtitle(Window *window, const char *id) {
	if (!window->home)
		window->home = load_cached_home();
	for (guint shelf = 0; window->home && shelf < window->home->shelves->len; shelf++) {
		GPtrArray *cards = ((Shelf *)g_ptr_array_index(window->home->shelves, shelf))->cards;
		for (guint index = 0; index < cards->len; index++) {
			Card *card = g_ptr_array_index(cards, index);
			if (g_str_equal(card->id, id))
				return card->subtitle;
		}
	}
	return NULL;
}


/* The page already slid in with placeholders, so it fills in where it is, or not at all once it was left */
static void fill_collection(Window *window, JSCValue *message, gboolean is_artist) {
	g_autofree char *id = read_text(message, "id");
	SongPage *page = find_song_page(window, id);
	if (!page || get_song_page_songs(page)) {
		if (g_strcmp0(id, window->waiting_card_id) == 0)
			stop_waiting(window);
		return;
	}
	g_autofree char *title = read_text(message, "title");
	g_autofree char *detail = read_text(message, "detail");
	g_autofree char *artwork = read_text(message, "artwork");
	AdwNavigationPage *navigation_page = adw_navigation_view_find_page(ADW_NAVIGATION_VIEW(window->navigation), id);
	adw_navigation_page_set_title(navigation_page, title);
	g_autofree char *album_artist = read_text(message, "artist");
	/* A mix YouTube made names no artist in its header, while its card on Home names the artists in it */
	const char *subtitle = is_artist || *album_artist ? NULL : find_home_subtitle(window, id);
	set_song_page_heading(page, title, subtitle && *subtitle ? subtitle : detail, artwork, is_artist);
	set_song_page_pin(page, id, title, artwork);
	if (is_artist)
		fetch_about(window, id, title, NULL);
	else
		fetch_about(window, id, album_artist, title);
	if (is_artist)
		add_cards(page, message, "albums", TEXT_ALBUMS_AND_SINGLES, FALSE);
	/* The page slid in already, so its contents crossfade in from the waiting shapes without moving it again */
	g_autoptr(GListModel) songs = create_song_store(message);
	g_object_set_data(G_OBJECT(songs), SONG_COUNT_KEY, GINT_TO_POINTER(read_number(message, "songCount")));
	set_song_page_songs(page, songs, FALSE, FALSE);
	update_song_page(page, TRUE);
	if (g_strcmp0(id, window->waiting_card_id) == 0)
		stop_waiting(window);
	if (window->reveal_video) {
		reveal_song_page_song(page, window->reveal_video);
		g_clear_pointer(&window->reveal_video, g_free);
	}
}


/* Home's heading carries the listener's own picture as a round glowing cover, with its colours behind */
static void show_home_heading(Window *window, SongPage *page) {
	set_song_page_heading(page, TEXT_HOME, TEXT_HOME_DETAIL, window->state.account_photo, window->state.account_photo != NULL);
}


/* YouTube Music gives a small picture whose address names its size, so a larger one is asked for in its place */
static void receive_account_photo(Window *window, JSCValue *message) {
	g_autofree char *photo = read_text(message, "photo");
	g_clear_pointer(&window->state.account_photo, g_free);
	if (*photo) {
		g_autoptr(GRegex) size = g_regex_new("=s[0-9]+", 0, 0, NULL);
		window->state.account_photo = g_regex_replace_literal(size, photo, -1, 0, ACCOUNT_PHOTO_SIZE, 0, NULL);
	}
	show_artwork(window->home_cover, window->state.account_photo ? window->state.account_photo : HOME_ARTWORK);
	SongPage *home = find_song_page(window, HOME_TAG);
	if (home)
		show_home_heading(window, home);
}


/* Songs played lately lead Home, newest first, after a card for the page that lists them all */
static Shelf *create_history_shelf(Home *home) {
	Shelf *shelf = g_new0(Shelf, 1);
	shelf->title = g_strdup(TEXT_HISTORY);
	shelf->cards = g_ptr_array_new_with_free_func(free_card);
	GPtrArray *songs = home->history;
	if (!songs->len)
		return shelf;
	Card *all = g_new0(Card, 1);
	all->id = g_strdup(home->history_id);
	all->title = g_strdup(TEXT_FULL_HISTORY);
	all->subtitle = g_strdup_printf(songs->len == 1 ? TEXT_SONG_COUNT_ONE : TEXT_SONG_COUNT, songs->len);
	all->artwork = g_strdup(HISTORY_ARTWORK);
	g_ptr_array_add(shelf->cards, all);
	/* A song played twice lately shows once, and plays from its newest play */
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint index = 0; index < songs->len && shelf->cards->len <= HISTORY_CARD_LIMIT; index++) {
		CrimsonSong *song = g_ptr_array_index(songs, index);
		if (!g_hash_table_add(seen, song->id))
			continue;
		Card *card = g_new0(Card, 1);
		card->id = g_strconcat(HISTORY_PREFIX, song->id, NULL);
		card->title = g_strdup(song->title);
		card->subtitle = g_strdup(song->artist);
		card->artwork = g_strdup(song->artwork);
		card->video_id = g_strdup(song->id);
		g_ptr_array_add(shelf->cards, card);
	}
	return shelf;
}


/* Home is shelves of cards, as YouTube Music shows them, above the songs it picks for the listener, and a fresh
 * answer changes only the shelves and songs that differ from what is shown */
static void show_home(Window *window, SongPage *page) {
	Home *home = window->home;
	if (!window->history)
		window->history = g_list_store_new(CRIMSON_TYPE_SONG);
	update_song_store(window->history, home->history);
	g_set_str(&window->history_id, home->history_id);
	g_autoptr(Shelf) history = create_history_shelf(home);
	g_autoptr(GPtrArray) shelves = g_ptr_array_new();
	g_ptr_array_add(shelves, history);
	g_ptr_array_extend(shelves, home->shelves, NULL, NULL);
	set_song_page_shelves(page, shelves);
	set_song_page_waits_for_cards(page, FALSE);
	GListStore *songs = G_LIST_STORE(get_song_page_songs(page));
	if (!songs) {
		g_autoptr(GListStore) store = g_list_store_new(CRIMSON_TYPE_SONG);
		set_song_page_songs(page, G_LIST_MODEL(store), FALSE, FALSE);
		songs = store;
	}
	update_song_store(songs, home->songs);
	update_song_page(page, TRUE);
}


/* Home opens with what it showed last, even from an earlier session, while YouTube is asked for it afresh, and
 * only waits with placeholders when nothing was ever shown */
static void open_home(Window *window) {
	SongPage *page = push_song_page(window, HOME_TAG, TEXT_HOME, FALSE);
	set_song_page_profile(page);
	show_home_heading(window, page);
	if (!window->home)
		window->home = load_cached_home();
	if (window->home) {
		show_home(window, page);
	} else {
		show_waiting(window, HOME_TAG);
		set_song_page_waits_for_cards(page, TRUE);
	}
	request_home(window->engine);
}


/* The Discover Mix leads Home with every mix kept before it, since YouTube shows only the mix it has now */
static Shelf *create_discover_shelf(Home *home) {
	Shelf *shelf = g_new0(Shelf, 1);
	shelf->title = g_strdup(TEXT_DISCOVER);
	shelf->cards = keep_discover_mix(home->discover_songs, home->discover);
	return shelf;
}


/* A kept Discover Mix opens like an album, straight from what was kept of it */
static void open_discover_mix(Window *window, const char *id) {
	g_autofree char *detail = NULL;
	g_autofree char *artwork = NULL;
	g_autoptr(GPtrArray) songs = load_discover_mix(id, &detail, &artwork);
	SongPage *page = push_song_page(window, id, TEXT_DISCOVER_MIX, TRUE);
	set_song_page_heading(page, TEXT_DISCOVER_MIX, detail, artwork, FALSE);
	set_song_page_pin(page, id, TEXT_DISCOVER_MIX, artwork);
	g_autoptr(GListStore) store = g_list_store_new(CRIMSON_TYPE_SONG);
	g_list_store_splice(store, 0, 0, songs->pdata, songs->len);
	set_song_page_songs(page, G_LIST_MODEL(store), FALSE, FALSE);
	update_song_page(page, TRUE);
}


/* The waiting ends only once the shelves are in, since a page without songs at that point counts as not found */
static void fill_home(Window *window, JSCValue *message) {
	g_clear_pointer(&window->home, free_home);
	window->home = read_home(message);
	if (window->home->discover)
		g_ptr_array_insert(window->home->shelves, 0, create_discover_shelf(window->home));
	save_cached_home(window->home);
	SongPage *page = find_song_page(window, HOME_TAG);
	if (page)
		show_home(window, page);
	if (g_strcmp0(HOME_TAG, window->waiting_card_id) == 0)
		stop_waiting(window);
}


/* A finished edit refreshes the library and the edited playlist, so names, order and songs match YouTube */
/* Liked Music and Episodes for Later have no suggestions, and dynamic playlists are this computer's own */
static gboolean has_suggestions(CrimsonPlaylist *playlist) {
	return playlist && get_settings()->shows_suggestions && is_own_playlist(playlist) && playlist->is_editable
		&& !g_str_has_prefix(playlist->id, DYNAMIC_PREFIX) && !is_local_playlist(playlist);
}


void show_suggestions(Window *window) {
	CrimsonPlaylist *playlist = window->shown;
	if (!has_suggestions(playlist)) {
		set_song_page_suggestions(window->playlist_page, NULL);
		return;
	}
	GListModel *suggestions = g_hash_table_lookup(window->suggestions, playlist->id);
	set_song_page_suggestions(window->playlist_page, suggestions);
	if (!suggestions && window->is_signed_in) {
		g_hash_table_insert(window->suggestions, g_strdup(playlist->id), g_list_store_new(CRIMSON_TYPE_SONG));
		request_suggestions(window->engine, playlist->id);
	}
}


static void fill_suggestions(Window *window, JSCValue *message) {
	g_autofree char *playlist_id = read_text(message, "playlist");
	g_autoptr(GListModel) songs = create_song_store(message);
	g_hash_table_insert(window->suggestions, g_steal_pointer(&playlist_id), g_steal_pointer(&songs));
	show_suggestions(window);
}


typedef struct {
	Window *window;
	CrimsonPlaylist *playlist;
} SuggestionMove;


/* The suggestion's row folds shut first, then the song slides open at the end of the playlist above */
static void finish_suggestion(CrimsonSong *song, gpointer data) {
	SuggestionMove *move = data;
	GListStore *suggestions = g_hash_table_lookup(move->window->suggestions, move->playlist->id);
	guint position;
	if (suggestions && g_list_store_find(suggestions, song, &position))
		g_list_store_remove(suggestions, position);
	add_song_to(move->window, move->playlist, song);
	g_object_unref(move->playlist);
	g_free(move);
}


static void add_suggestion(CrimsonSong *song, gpointer data) {
	Window *window = data;
	if (!window->shown)
		return;
	SuggestionMove *move = g_new(SuggestionMove, 1);
	*move = (SuggestionMove){window, g_object_ref(window->shown)};
	fold_song_away(song, finish_suggestion, move);
}


static void finish_edit(Window *window, JSCValue *message) {
	g_autoptr(JSCValue) ok = jsc_value_object_get_property(message, "ok");
	if (!jsc_value_to_boolean(ok))
		show_message(window, TEXT_EDIT_FAILED);
	g_autofree char *id = read_text(message, "playlist");
	g_autofree char *video_id = read_text(message, "video");
	g_autofree char *set_id = read_text(message, "setId");
	request_library(window->engine);
	CrimsonPlaylist *playlist = find_playlist(window, id);
	if (!playlist)
		return;
	/* A failed edit lets go of every edit waiting on the playlist and asks YouTube for it as it really is */
	if (!jsc_value_to_boolean(ok)) {
		forget_playlist_edits(playlist);
		refresh_playlist(window, playlist);
		return;
	}
	/* An added song takes its entry id from the answer, which removing or moving it needs, and the waiting copy is
	 * the same song as the listed one */
	for (guint index = 0; *set_id && index < playlist->pending_added->len; index++) {
		CrimsonSong *song = g_ptr_array_index(playlist->pending_added, index);
		if (g_strcmp0(song->id, video_id) == 0 && (!song->set_id || !*song->set_id)) {
			g_set_str(&song->set_id, set_id);
			if (playlist->has_songs)
				save_cached_songs(playlist);
			break;
		}
	}
}


void update_liked(Window *window) {
	CrimsonPlaylist *liked = find_playlist(window, LIKED_PLAYLIST);
	if (liked && liked->has_songs) {
		g_hash_table_remove_all(window->liked_videos);
		for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(liked->songs)); index++) {
			g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(liked->songs), index);
			g_hash_table_add(window->liked_videos, g_strdup(song->id));
		}
	}
	set_liked_videos(window->liked_videos);
	show_player_liked(window->bar, is_video_liked(window->current_video));
	if (liked && liked->has_songs)
		gather_song_info(G_LIST_MODEL(liked->songs));
	update_dynamic_playlists(window);
}


static void show_video_disliked(Window *window, const char *video_id, gboolean is_disliked) {
	set_video_disliked(video_id, is_disliked);
	update_disliked_rows();
	show_player_disliked(window->bar, is_video_disliked(window->current_video));
}


/* Turns the heart at once and asks YouTube, which turns it back with a message if it refuses */
static void toggle_video_like(Window *window, const char *video_id, CrimsonSong *song) {
	if (is_local_song(video_id)) {
		show_message(window, TEXT_LOCAL_NOT_ON_YOUTUBE);
		return;
	}
	if (!require_account(window, TEXT_FEATURE_LIKE))
		return;
	gboolean is_liked = !g_hash_table_contains(window->liked_videos, video_id);
	if (is_liked)
		g_hash_table_add(window->liked_videos, g_strdup(video_id));
	else
		g_hash_table_remove(window->liked_videos, video_id);
	/* A like takes the place of a dislike on YouTube */
	if (is_liked && is_video_disliked(video_id))
		show_video_disliked(window, video_id, FALSE);
	set_liked_videos(window->liked_videos);
	show_player_liked(window->bar, is_video_liked(window->current_video));
	like_song(window->engine, video_id, is_liked);
}


/* Turns the thumb at once, takes a like away as YouTube does, and moves on from the playing song once disliked */
static void toggle_video_dislike(Window *window, const char *video_id) {
	if (is_local_song(video_id)) {
		show_message(window, TEXT_LOCAL_NOT_ON_YOUTUBE);
		return;
	}
	if (!require_account(window, TEXT_FEATURE_LIKE))
		return;
	gboolean is_disliked = !is_video_disliked(video_id);
	if (is_disliked && g_hash_table_remove(window->liked_videos, video_id)) {
		set_liked_videos(window->liked_videos);
		show_player_liked(window->bar, is_video_liked(window->current_video));
	}
	show_video_disliked(window, video_id, is_disliked);
	dislike_song(window->engine, video_id, is_disliked);
	if (is_disliked && g_strcmp0(video_id, window->current_video) == 0)
		play_next(NULL, NULL, window);
}


static void toggle_song_dislike(CrimsonSong *song, gpointer data) {
	toggle_video_dislike(data, song->id);
}


void toggle_playing_dislike(gpointer data) {
	Window *window = data;
	if (window->current_video)
		toggle_video_dislike(window, window->current_video);
}


/* YouTube refusing turns the thumb back, and a dislike that took a like away changes Liked Music */
static void finish_dislike(Window *window, JSCValue *message) {
	g_autofree char *video = read_text(message, "video");
	g_autoptr(JSCValue) ok = jsc_value_object_get_property(message, "ok");
	if (jsc_value_to_boolean(ok)) {
		refresh_playlist(window, find_playlist(window, LIKED_PLAYLIST));
		return;
	}
	g_autoptr(JSCValue) disliked = jsc_value_object_get_property(message, "disliked");
	if (jsc_value_to_boolean(disliked) == is_video_disliked(video))
		show_video_disliked(window, video, !jsc_value_to_boolean(disliked));
	show_message(window, TEXT_DISLIKE_FAILED);
}


static void toggle_song_love_event(CrimsonSong *song, gpointer data) {
	toggle_song_love(song);
}


static void toggle_song_like(CrimsonSong *song, gpointer data) {
	toggle_video_like(data, song->id, song);
}


void toggle_playing_love(gpointer data) {
	Window *window = data;
	CrimsonSong *song = get_queue_song(window->queue);
	if (song)
		toggle_song_love(song);
}


void toggle_playing_like(gpointer data) {
	Window *window = data;
	if (window->current_video)
		toggle_video_like(window, window->current_video, get_queue_song(window->queue));
}


static void finish_like(Window *window, JSCValue *message) {
	g_autoptr(JSCValue) ok = jsc_value_object_get_property(message, "ok");
	if (jsc_value_to_boolean(ok)) {
		refresh_playlist(window, find_playlist(window, LIKED_PLAYLIST));
		return;
	}
	g_autofree char *video = read_text(message, "video");
	g_autoptr(JSCValue) liked = jsc_value_object_get_property(message, "liked");
	if (jsc_value_to_boolean(liked) == g_hash_table_contains(window->liked_videos, video))
		toggle_video_like(window, video, NULL);
	show_message(window, TEXT_LIKE_FAILED);
}


/* Opens the album a lookup found, or says there is none, unless the listener has moved on to another page */
static void show_found_album(Window *window, JSCValue *message) {
	g_autofree char *video = read_text(message, "video");
	if (g_strcmp0(video, window->waiting_card_id) != 0)
		return;
	stop_waiting(window);
	g_autofree char *id = read_text(message, "id");
	if (*id)
		open_card(id, window);
	else
		show_message(window, TEXT_NO_ALBUM);
}


static void receive_lyrics(Window *window, JSCValue *message);
static void fill_home(Window *window, JSCValue *message);
static void receive_account_photo(Window *window, JSCValue *message);
static void receive_comments(Window *window, JSCValue *message);
static void receive_replies(Window *window, JSCValue *message);


void receive_answer(const char *type, JSCValue *message, gpointer data) {
	Window *window = data;
	if (g_str_equal(type, "search")) {
		fill_search(window, message);
	} else if (g_str_equal(type, "account")) {
		receive_account_photo(window, message);
	} else if (g_str_equal(type, "home")) {
		fill_home(window, message);
	} else if (g_str_equal(type, "artist") || g_str_equal(type, "album")) {
		fill_collection(window, message, g_str_equal(type, "artist"));
	} else if (g_str_equal(type, "similar-songs") || g_str_equal(type, "similar-artists")) {
		fill_similar(window, message, g_str_equal(type, "similar-artists"));
	} else if (g_str_equal(type, "artist-more")) {
		fill_more(window, message);
	} else if (g_str_equal(type, "suggestions")) {
		fill_suggestions(window, message);
	} else if (g_str_equal(type, "album-found")) {
		show_found_album(window, message);
	} else if (g_str_equal(type, "liked")) {
		finish_like(window, message);
	} else if (g_str_equal(type, "disliked")) {
		finish_dislike(window, message);
	} else if (g_str_equal(type, "created")) {
		g_free(window->new_playlist_id);
		window->new_playlist_id = read_text(message, "id");
		request_library(window->engine);
	} else if (g_str_equal(type, "edited")) {
		finish_edit(window, message);
	} else if (g_str_equal(type, "lyrics")) {
		receive_lyrics(window, message);
	} else if (g_str_equal(type, "comments")) {
		receive_comments(window, message);
	} else if (g_str_equal(type, "replies")) {
		receive_replies(window, message);
	}
}


/* Right click menus ======================================================= */


/* Menus open from an invisible menu button moved to the click, since only a menu button gives its popover the
 * height it needs, and clicks in the menu then never reach the row underneath.
 * Submenus open beside the menu as on other desktops, rather than sliding over it */
static void show_menu(Window *window, GMenuModel *menu, GtkWidget *row, double x, double y) {
	GtkWidget *anchor = window->menu_anchor;
	graphene_point_t point = GRAPHENE_POINT_INIT((float)x, (float)y);
	if (!gtk_widget_compute_point(row, gtk_widget_get_parent(anchor), &point, &point))
		return;
	gtk_widget_set_margin_start(anchor, (int)point.x);
	gtk_widget_set_margin_top(anchor, (int)point.y);
	GtkWidget *popover = gtk_popover_menu_new_from_model_full(menu, GTK_POPOVER_MENU_NESTED);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){0, 0, 1, 1});
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(anchor), popover);
	gtk_menu_button_popup(GTK_MENU_BUTTON(anchor));
}


GtkWidget *create_menu_anchor(Window *window) {
	window->menu_anchor = gtk_menu_button_new();
	gtk_widget_set_halign(window->menu_anchor, GTK_ALIGN_START);
	gtk_widget_set_valign(window->menu_anchor, GTK_ALIGN_START);
	gtk_widget_set_size_request(window->menu_anchor, 1, 1);
	gtk_widget_set_opacity(window->menu_anchor, 0);
	gtk_widget_set_can_target(window->menu_anchor, FALSE);
	gtk_widget_set_can_focus(window->menu_anchor, FALSE);
	gtk_widget_add_css_class(window->menu_anchor, "menu-anchor");
	gtk_menu_button_set_child(GTK_MENU_BUTTON(window->menu_anchor), adw_bin_new());
	return window->menu_anchor;
}


static void append_target(GMenu *menu, const char *label, const char *action, const char *target) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(target));
	g_menu_append_item(menu, item);
}


static gboolean is_showing_playlist(Window *window) {
	AdwNavigationPage *page = adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(window->navigation));
	return g_strcmp0(adw_navigation_page_get_tag(page), PLAYLIST_TAG) == 0;
}


static void append_lastfm_page(GMenu *menu, const char *label, const char *address) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, "win.open-address", g_variant_new_string(address));
	g_menu_append_item(menu, item);
}


/* Last.fm's pages of the song, its artist and album, the listener's own plays, and the love, which needs the
 * connection */
static void append_song_lastfm_menu(GMenu *menu, CrimsonSong *song) {
	if (!is_lastfm_available())
		return;
	g_autoptr(GMenu) lastfm = g_menu_new();
	g_autofree char *artist = get_song_lastfm_artist(song);
	g_autofree char *track = make_lastfm_address(artist, NULL, song->title);
	g_autofree char *artist_page = make_lastfm_address(artist, NULL, NULL);
	g_autoptr(GMenu) pages = g_menu_new();
	append_lastfm_page(pages, TEXT_LASTFM_SHOW_SONG, track);
	append_lastfm_page(pages, TEXT_LASTFM_SHOW_ARTIST, artist_page);
	if (*song->album) {
		g_autofree char *album = make_lastfm_address(artist, song->album, NULL);
		append_lastfm_page(pages, TEXT_LASTFM_SHOW_ALBUM, album);
	}
	g_menu_append_section(lastfm, NULL, G_MENU_MODEL(pages));
	if (get_lastfm_profile()) {
		g_autoptr(GMenu) own = g_menu_new();
		g_autofree char *library = g_strconcat(get_lastfm_profile(), LASTFM_LIBRARY_PATH,
			track + strlen(LASTFM_MUSIC_ADDRESS), NULL);
		append_lastfm_page(own, TEXT_LASTFM_YOUR_PLAYS, library);
		g_menu_append(own, is_song_loved(song) ? TEXT_UNLOVE : TEXT_LOVE, "win.love-song");
		g_menu_append_section(lastfm, NULL, G_MENU_MODEL(own));
	}
	g_menu_append_submenu(menu, TEXT_LASTFM, G_MENU_MODEL(lastfm));
}


/* An artist or album card, or a pin, leads to the same on Last.fm. Album cards name their artist only in a line
 * of mixed details, so an album is looked up by Last.fm's search */
static void append_similar_artists(GMenu *menu, const char *id, const char *name) {
	g_autofree char *target = g_strconcat(id ? id : "", "\n", name, NULL);
	append_target(menu, TEXT_SIMILAR_ARTISTS, "win.similar-artists", target);
}


static void append_card_lastfm_menu(GMenu *menu, const char *id, const char *title) {
	if (title && g_str_has_prefix(id, ARTIST_PREFIX))
		append_similar_artists(menu, id, title);
	if (!is_lastfm_available() || !title)
		return;
	g_autoptr(GMenu) lastfm = g_menu_new();
	if (!g_str_has_prefix(id, ARTIST_PREFIX)) {
		g_autofree char *query = g_uri_escape_string(title, NULL, FALSE);
		g_autofree char *search = g_strconcat(LASTFM_ALBUM_SEARCH, query, NULL);
		append_lastfm_page(lastfm, TEXT_LASTFM_SHOW_ALBUM, search);
	} else {
		g_autofree char *artist = make_lastfm_address(title, NULL, NULL);
		append_lastfm_page(lastfm, TEXT_LASTFM_SHOW_ARTIST, artist);
	}
	g_menu_append_submenu(menu, TEXT_LASTFM, G_MENU_MODEL(lastfm));
}


/* A song by several artists always offers them by name, leaving out the one whose page is on screen */
static void append_song_artists(GMenu *menu, CrimsonSong *song, const char *shown) {
	g_auto(GStrv) artists = list_song_artists(song);
	if (!artists[0] || (!artists[2] && g_strcmp0(shown, artists[0]) == 0))
		return;
	if (!artists[2]) {
		append_target(menu, TEXT_GO_TO_ARTIST, "win.open-card", artists[0]);
		return;
	}
	g_autoptr(GMenu) choices = g_menu_new();
	for (guint index = 0; artists[index]; index += 2) {
		if (g_strcmp0(shown, artists[index]) != 0)
			append_target(choices, artists[index + 1], "win.open-card", artists[index]);
	}
	g_menu_append_submenu(menu, TEXT_GO_TO_ARTIST, G_MENU_MODEL(choices));
}


static GMenu *create_song_menu(Window *window, CrimsonSong *song) {
	g_set_object(&window->menu_song, song);
	GMenu *menu = g_menu_new();
	/* With songs already queued, adding to the end of the queue comes first and playing next follows it */
	if (g_strcmp0(song->id, window->current_video) != 0 && is_upcoming_queued(window->queue, 1))
		g_menu_append(menu, TEXT_ADD_TO_QUEUE, "win.add-to-queue");
	if (g_strcmp0(song->id, window->current_video) != 0)
		g_menu_append(menu, TEXT_PLAY_NEXT, "win.play-next");
	/* The page on screen is not offered again, so an album's songs only lead on to their artist */
	AdwNavigationPage *page = adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(window->navigation));
	const char *shown = page ? adw_navigation_page_get_tag(page) : NULL;
	append_song_artists(menu, song, shown);
	if (*song->album_id && g_strcmp0(shown, song->album_id) != 0)
		append_target(menu, TEXT_GO_TO_ALBUM, "win.open-card", song->album_id);
	else if (!*song->album_id && !is_local_song(song->id))
		append_target(menu, TEXT_GO_TO_ALBUM, "win.find-album", song->id);
	if (is_local_song(song->id))
		append_target(menu, TEXT_OPEN_LOCATION, "win.open-location", song->id);
	else
		append_target(menu, TEXT_SHARE, "win.share", song->id);
	/* A local file names its artist and album only as text, so they are found by searching for them */
	/* Quotes keep the artist's name together, which finds that artist better than the loose words */
	if (is_local_song(song->id) && *song->artist) {
		g_autofree char *artist = g_strdup_printf("\"%s\"", song->artist);
		append_target(menu, TEXT_SEARCH_FOR_ARTIST, "win.search-for", artist);
	}
	if (is_local_song(song->id) && *song->album) {
		g_autofree char *album = *song->artist ? g_strdup_printf("%s %s", song->album, song->artist)
			: g_strdup(song->album);
		append_target(menu, TEXT_SEARCH_FOR_ALBUM, "win.search-for", album);
	}
	g_menu_append(menu, TEXT_SIMILAR_SONGS, "win.similar-songs");
	g_autofree char *artist = get_song_lastfm_artist(song);
	g_autofree char *artist_id = get_song_artist_id(song);
	append_similar_artists(menu, artist_id, artist);
	g_autoptr(GMenu) playlists = g_menu_new();
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->playlists)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		gboolean is_local = is_local_playlist(playlist);
		if (is_own_playlist(playlist) && (is_local || (window->is_signed_in && !is_local_song(song->id))))
			append_target(playlists, playlist->title, "win.add-to-playlist", playlist->id);
	}
	/* A new local playlist can always be made from here, so the list is never a dead end */
	g_autoptr(GMenu) making = g_menu_new();
	g_menu_append(making, TEXT_NEW_LOCAL_PLAYLIST_WITH_SONG, "win.new-local-playlist-with-song");
	g_menu_append_section(playlists, NULL, G_MENU_MODEL(making));
	g_menu_append_submenu(menu, TEXT_ADD_TO_PLAYLIST, G_MENU_MODEL(playlists));
	if (is_showing_playlist(window) && window->shown && window->shown->is_editable && !is_suggested_song(song))
		g_menu_append(menu, TEXT_REMOVE_FROM_PLAYLIST, "win.remove-from-playlist");
	/* A local song found in several formats lets the listener pick which one plays, the current one marked */
	g_autoptr(GPtrArray) versions = is_local_song(song->id) ? list_local_versions(song->id) : NULL;
	if (versions && versions->len > 1) {
		g_autoptr(GMenu) choices = g_menu_new();
		for (guint index = 0; index < versions->len; index++) {
			const char *version = g_ptr_array_index(versions, index);
			g_autofree char *label = describe_local_version(version);
			append_target(choices, label, "win.local-version", version);
		}
		g_simple_action_set_state(window->version_action, g_variant_new_string(song->id));
		g_autofree char *title = g_strdup_printf(TEXT_VERSIONS, versions->len);
		g_menu_append_submenu(menu, title, G_MENU_MODEL(choices));
	}
	g_autoptr(GMenu) information = g_menu_new();
	append_song_lastfm_menu(information, song);
	g_menu_append(information, TEXT_INFORMATION, "win.song-information");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(information));
	return menu;
}


void open_song_menu(CrimsonSong *song, guint position, GtkWidget *row, double x, double y, gpointer data) {
	Window *window = data;
	g_autoptr(GMenu) menu = create_song_menu(window, song);
	show_menu(window, G_MENU_MODEL(menu), row, x, y);
}


/* A song card from the listener's playlists gets the song's menu and a way to its playlist */
static void open_card_menu(const char *id, GtkWidget *card, double x, double y, gpointer data) {
	Window *window = data;
	guint position = 0;
	g_autoptr(CrimsonSong) played = g_str_has_prefix(id, HISTORY_PREFIX) ? find_history_song(window, id, &position)
		: NULL;
	if (played) {
		g_autoptr(GMenu) menu = create_song_menu(window, played);
		show_menu(window, G_MENU_MODEL(menu), card, x, y);
		return;
	}
	CrimsonPlaylist *playlist = g_str_has_prefix(id, LIBRARY_PREFIX) ? find_library_song(window, id, &position) : NULL;
	if (!playlist) {
		Card *shown = g_object_get_data(G_OBJECT(card), "card");
		g_autoptr(GMenu) menu = g_menu_new();
		append_card_lastfm_menu(menu, id, shown ? shown->title : NULL);
		if (g_menu_model_get_n_items(G_MENU_MODEL(menu)))
			show_menu(window, G_MENU_MODEL(menu), card, x, y);
		return;
	}
	g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(playlist->songs), position);
	g_autoptr(GMenu) menu = create_song_menu(window, song);
	g_autoptr(GMenuItem) item = g_menu_item_new(TEXT_GO_TO_PLAYLIST, NULL);
	g_menu_item_set_action_and_target_value(item, "win.go-to-playlist", g_variant_new_string(id));
	g_menu_prepend_item(menu, item);
	show_menu(window, G_MENU_MODEL(menu), card, x, y);
}


void open_playlist_menu(Window *window, CrimsonPlaylist *playlist, GtkWidget *row, double x, double y) {
	g_set_object(&window->menu_playlist, playlist);
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_PLAY, "win.play-playlist");
	g_menu_append(menu, TEXT_RANDOM, "win.play-playlist-random");
	g_autoptr(GMenu) editing = g_menu_new();
	if (is_own_playlist(playlist)) {
		g_menu_append(editing, TEXT_RENAME, "win.rename-playlist");
		g_menu_append(editing, TEXT_DELETE, "win.delete-playlist");
	}
	g_menu_append(editing, TEXT_NEW_PLAYLIST, "win.new-playlist");
	g_menu_append(editing, TEXT_NEW_LOCAL_PLAYLIST, "win.new-local-playlist");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(editing));
	show_menu(window, G_MENU_MODEL(menu), row, x, y);
}


void open_pin_menu(Window *window, const char *id, GtkWidget *widget, double x, double y) {
	g_autoptr(GMenu) menu = g_menu_new();
	for (guint index = 0; index < window->pins->len; index++) {
		Card *pin = g_ptr_array_index(window->pins, index);
		if (g_str_equal(pin->id, id))
			append_card_lastfm_menu(menu, id, pin->title);
	}
	append_target(menu, TEXT_UNPIN, "win.unpin", id);
	show_menu(window, G_MENU_MODEL(menu), widget, x, y);
}


void open_dynamic_menu(Window *window, const char *id, GtkWidget *widget, double x, double y) {
	g_set_str(&window->menu_dynamic_id, id);
	g_autoptr(GMenu) menu = g_menu_new();
	append_target(menu, TEXT_EDIT_FILTERS, "win.edit-dynamic", id);
	g_menu_append(menu, TEXT_DELETE, "win.delete-dynamic");
	show_menu(window, G_MENU_MODEL(menu), widget, x, y);
}


void open_sidebar_menu(Window *window, GtkWidget *widget, double x, double y) {
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_NEW_PLAYLIST, "win.new-playlist");
	g_menu_append(menu, TEXT_NEW_LOCAL_PLAYLIST, "win.new-local-playlist");
	g_menu_append(menu, TEXT_NEW_DYNAMIC, "win.new-dynamic");
	show_menu(window, G_MENU_MODEL(menu), widget, x, y);
}


/* Moves the song on screen at once and tells YouTube which entry it now sits before */
static void move_song(guint from, guint to, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = window->shown;
	if (!playlist || !playlist->is_editable)
		return;
	g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(playlist->songs), from);
	g_list_store_remove(playlist->songs, from);
	g_list_store_insert(playlist->songs, to, song);
	g_autoptr(CrimsonSong) successor = g_list_model_get_item(G_LIST_MODEL(playlist->songs), to + 1);
	if (!is_local_playlist(playlist))
		move_in_playlist(window->engine, playlist->id, song->set_id, successor ? successor->set_id : "");
	save_playlist(window, playlist);
}


const SongPageEvents SONG_PAGE_EVENTS = {
	play_songs,
	choose_song,
	open_card,
	open_card_menu,
	{open_song_menu, move_song, toggle_song_like, toggle_song_dislike, toggle_song_love_event, add_suggestion},
};


/* Menu actions ============================================================ */


/* Queues the song right after the playing one, or after the songs queued before it, or plays it when nothing
 * plays */
static void queue_song(Window *window, CrimsonSong *song, gboolean is_last) {
	if (!window->current_video) {
		g_autoptr(GListStore) single = g_list_store_new(CRIMSON_TYPE_SONG);
		g_list_store_append(single, song);
		play_songs(G_LIST_MODEL(single), 0, FALSE, window);
		return;
	}
	queue_song_next(window->queue, song, is_last);
	show_upcoming(window);
}


/* A song dropped on the upcoming covers joins the end of the queue */
void queue_dropped_song(CrimsonSong *song, gpointer data) {
	queue_song(data, song, TRUE);
}


static void play_next_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->menu_song)
		queue_song(window, window->menu_song, FALSE);
}


static void add_to_queue(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->menu_song)
		queue_song(window, window->menu_song, TRUE);
}


static void open_card_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	open_card(g_variant_get_string(parameter, NULL), data);
}


static void find_album_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *video = g_variant_get_string(parameter, NULL);
	show_waiting(window, video);
	find_album(window->engine, video);
}


/* A copy of the song, which slides open where it joins a list */
static CrimsonSong *copy_added_song(CrimsonSong *song) {
	CrimsonSong *copy = g_object_new(CRIMSON_TYPE_SONG, NULL);
	copy->id = g_strdup(song->id);
	copy->title = g_strdup(song->title);
	copy->artist = g_strdup(song->artist);
	copy->artist_id = g_strdup(song->artist_id);
	copy->album = g_strdup(song->album);
	copy->album_id = g_strdup(song->album_id);
	copy->duration = g_strdup(song->duration);
	copy->artwork = g_strdup(song->artwork);
	copy->set_id = g_strdup("");
	mark_song_arriving(copy);
	return copy;
}


static void append_song_copy(CrimsonPlaylist *playlist, CrimsonSong *song) {
	g_autoptr(CrimsonSong) copy = copy_added_song(song);
	g_list_store_append(playlist->songs, copy);
}


/* Local playlists take any song and need no account, while YouTube playlists take only YouTube songs */
void add_song_to(Window *window, CrimsonPlaylist *playlist, CrimsonSong *song) {
	if (is_local_playlist(playlist)) {
		append_song_copy(playlist, song);
		save_playlist(window, playlist);
		g_autofree char *text = g_strdup_printf(TEXT_ADDED_TO, playlist->title);
		show_message(window, text);
		return;
	}
	if (is_local_song(song->id)) {
		show_message(window, TEXT_LOCAL_NOT_ON_YOUTUBE);
		return;
	}
	if (!require_account(window, TEXT_FEATURE_ADD))
		return;
	if (g_str_equal(playlist->id, LIKED_PLAYLIST)) {
		if (!is_video_liked(song->id))
			toggle_video_like(window, song->id, song);
		return;
	}
	if (!is_own_playlist(playlist))
		return;
	add_to_playlist(window->engine, playlist->id, song->id);
	/* The song waits on the playlist until YouTube lists it, so it shows however soon the playlist is opened */
	g_autoptr(CrimsonSong) copy = copy_added_song(song);
	note_playlist_addition(playlist, copy);
	if (playlist->has_songs)
		g_list_store_append(playlist->songs, copy);
	g_autofree char *text = g_strdup_printf(TEXT_ADDED_TO, playlist->title);
	show_message(window, text);
}


static void add_song_to_playlist(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = find_playlist(window, g_variant_get_string(parameter, NULL));
	if (window->menu_song && playlist)
		add_song_to(window, playlist, window->menu_song);
}


static void show_song_information(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->menu_song)
		show_info_dialog(GTK_WIDGET(window->window), window->menu_song);
}


/* The cover in the player bar opens the playing song's information */
static void show_playing_information(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonSong *song = window->current_video ? get_queue_song(window->queue) : NULL;
	if (song)
		show_info_dialog(GTK_WIDGET(window->window), song);
}


static void pick_local_version(GSimpleAction *action, GVariant *parameter, gpointer data) {
	g_simple_action_set_state(action, parameter);
	choose_local_version(g_variant_get_string(parameter, NULL));
}


/* The file manager opens the folder with the file picked out */
static void open_location(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_autoptr(GFile) file = g_file_new_for_uri(g_variant_get_string(parameter, NULL));
	g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
	gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(window->window), NULL, NULL, NULL);
}


/* A YouTube song opens its YouTube Music page in the browser, and a local file shows where it is kept */
static void share_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *id = g_variant_get_string(parameter, NULL);
	if (is_local_song(id)) {
		g_action_group_activate_action(G_ACTION_GROUP(window->window), "open-location", parameter);
		return;
	}
	g_autofree char *escaped = g_uri_escape_string(id, NULL, FALSE);
	g_autofree char *address = g_strconcat(SONG_PAGE_ADDRESS, escaped, NULL);
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(address);
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(window->window), NULL, NULL, NULL);
}


/* The search field shows what is searched for, and the search runs at once rather than after the typing pause */
static void search_for(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	gtk_editable_set_text(GTK_EDITABLE(window->search_entry), g_variant_get_string(parameter, NULL));
	start_search(GTK_SEARCH_ENTRY(window->search_entry), window);
}


static void needs_account(GSimpleAction *action, GVariant *parameter, gpointer data) {
	require_account(data, g_variant_get_string(parameter, NULL));
}


static void new_dynamic_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	edit_dynamic_playlist(data, NULL);
}


static void edit_dynamic_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	edit_dynamic_playlist(data, g_variant_get_string(parameter, NULL));
}


static void finish_dynamic_delete(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "confirm")
		&& window->menu_dynamic_id)
		delete_dynamic_playlist(window, window->menu_dynamic_id);
}


/* The filters are kept nowhere else, so deleting asks first */
static void ask_dynamic_delete(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = find_dynamic_playlist(window, window->menu_dynamic_id);
	if (!playlist)
		return;
	g_autofree char *heading = g_strdup_printf(TEXT_DELETE_PLAYLIST, playlist->title);
	AdwDialog *dialog = adw_alert_dialog_new(heading, TEXT_DELETE_DYNAMIC_DETAIL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "confirm", TEXT_DELETE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window->window), NULL, finish_dynamic_delete, window);
}


/* Similar ================================================================== */


typedef struct {
	Window *window;
	int token;
	char *id;
} SimilarWait;


static SongPage *open_similar_page(Window *window, const char *tag, const char *title, const char *detail,
	const char *artwork, gboolean is_round, gboolean *is_new) {
	/* Similar artists show as cards, and similar songs as a list alone */
	SongPage *page = open_song_page(window, tag, title, !is_round, is_new);
	if (*is_new) {
		set_song_page_heading(page, title, detail, artwork, is_round);
		g_set_str(&window->similar_tag, tag);
		window->similar_token++;
	}
	return page;
}


static void request_songs(char **found, gpointer data) {
	SimilarWait *wait = data;
	request_similar_songs(wait->window->engine, wait->token, wait->id, found);
	g_free(wait->id);
	g_free(wait);
}


static void request_artists(char **found, gpointer data) {
	SimilarWait *wait = data;
	request_similar_artists(wait->window->engine, wait->token, wait->id, found);
	g_free(wait->id);
	g_free(wait);
}


/* Songs like the one chosen, which Last.fm suggests and YouTube Music's radio of the song adds to */
static void show_similar_songs(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_autoptr(CrimsonSong) song = window->menu_song ? g_object_ref(window->menu_song) : NULL;
	if (!song)
		return;
	g_autofree char *tag = g_strconcat(SIMILAR_SONGS_TAG, song->id, NULL);
	g_autofree char *title = g_strdup_printf(TEXT_SIMILAR_TO, song->title);
	gboolean is_new = FALSE;
	open_similar_page(window, tag, title, song->artist, song->artwork, FALSE, &is_new);
	if (!is_new)
		return;
	SimilarWait *wait = g_new0(SimilarWait, 1);
	wait->window = window;
	wait->token = window->similar_token;
	wait->id = g_strdup(song->id);
	g_autofree char *artist = get_song_lastfm_artist(song);
	fetch_lastfm_similar(artist, song->title, request_songs, wait);
}


/* Artists like one, which Last.fm suggests and the artist's own page on YouTube Music adds to */
static void show_similar_artists(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_auto(GStrv) parts = g_strsplit(g_variant_get_string(parameter, NULL), "\n", 2);
	if (!parts[0] || !parts[1])
		return;
	g_autofree char *tag = g_strconcat(SIMILAR_ARTISTS_TAG, *parts[0] ? parts[0] : parts[1], NULL);
	g_autofree char *title = g_strdup_printf(TEXT_ARTISTS_LIKE, parts[1]);
	gboolean is_new = FALSE;
	SongPage *page = open_similar_page(window, tag, title, NULL, NULL, TRUE, &is_new);
	set_song_page_playable(page, FALSE);
	g_object_set_data_full(G_OBJECT(get_song_page_widget(page)), "similar-title", g_strdup(title), g_free);
	if (!is_new)
		return;
	SimilarWait *wait = g_new0(SimilarWait, 1);
	wait->window = window;
	wait->token = window->similar_token;
	wait->id = g_strdup(parts[0]);
	fetch_lastfm_similar(parts[1], NULL, request_artists, wait);
}


static void love_song_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->menu_song)
		toggle_song_love(window->menu_song);
}


static void unpin_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	unpin_card(data, g_variant_get_string(parameter, NULL));
}


/* A song dropped on the playlist on screen joins it, unless it is there already or the playlist cannot take it */
static gboolean takes_song(Window *window, const GValue *value) {
	CrimsonPlaylist *playlist = window->shown;
	if (!value || !G_VALUE_HOLDS(value, CRIMSON_TYPE_SONG) || !playlist || !is_showing_playlist(window))
		return FALSE;
	if (!g_str_equal(playlist->id, LIKED_PLAYLIST) && !(is_own_playlist(playlist) && playlist->is_editable))
		return FALSE;
	CrimsonSong *song = g_value_get_object(value);
	if (is_local_song(song->id) && !is_local_playlist(playlist))
		return FALSE;
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(playlist->songs)); index++) {
		g_autoptr(CrimsonSong) kept = g_list_model_get_item(G_LIST_MODEL(playlist->songs), index);
		if (g_strcmp0(kept->id, song->id) == 0)
			return FALSE;
	}
	return TRUE;
}


static void check_page_drop(GtkDropTarget *target, GParamSpec *spec, Window *window) {
	const GValue *value = gtk_drop_target_get_value(target);
	GtkWidget *page = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	if (!value || !takes_song(window, value)) {
		if (value)
			gtk_drop_target_reject(target);
		gtk_widget_remove_css_class(page, "drop-target");
	} else {
		gtk_widget_add_css_class(page, "drop-target");
	}
}


static gboolean drop_on_page(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
	if (!takes_song(window, value))
		return FALSE;
	add_song_to(window, window->shown, g_value_get_object(value));
	return TRUE;
}


static void leave_page(GtkDropTarget *target, Window *window) {
	gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
}


static void add_page_drop(Window *window) {
	GtkDropTarget *target = gtk_drop_target_new(CRIMSON_TYPE_SONG, GDK_ACTION_COPY);
	gtk_drop_target_set_preload(target, TRUE);
	g_signal_connect(target, "notify::value", G_CALLBACK(check_page_drop), window);
	g_signal_connect(target, "drop", G_CALLBACK(drop_on_page), window);
	g_signal_connect(target, "leave", G_CALLBACK(leave_page), window);
	gtk_widget_add_controller(get_song_page_widget(window->playlist_page), GTK_EVENT_CONTROLLER(target));
}


/* The song leaves the list once its row has folded, wherever the list has moved it by then */
typedef struct {
	Window *window;
	CrimsonPlaylist *playlist;
} Removal;


static void finish_removal(CrimsonSong *song, gpointer data) {
	Removal *removal = data;
	guint position;
	if (g_list_store_find(removal->playlist->songs, song, &position)) {
		g_list_store_remove(removal->playlist->songs, position);
		save_playlist(removal->window, removal->playlist);
	}
	g_object_unref(removal->playlist);
	g_free(removal);
}


static void remove_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = window->shown;
	guint position = 0;
	if (!window->menu_song || !playlist || !g_list_store_find(playlist->songs, window->menu_song, &position))
		return;
	if (!is_local_playlist(playlist)) {
		remove_from_playlist(window->engine, playlist->id, window->menu_song->id, window->menu_song->set_id);
		note_playlist_removal(playlist, window->menu_song);
	}
	Removal *removal = g_new(Removal, 1);
	*removal = (Removal){window, g_object_ref(playlist)};
	fold_song_away(window->menu_song, finish_removal, removal);
}


/* Playing a playlist from its menu opens it too, and plays once its songs are known */
static void play_menu_playlist(Window *window, gboolean is_shuffled) {
	CrimsonPlaylist *playlist = window->menu_playlist;
	if (!playlist)
		return;
	select_playlist_row(window, playlist->id);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
	g_clear_pointer(&window->waiting_play_id, g_free);
	guint count = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
	if (count)
		play_songs(G_LIST_MODEL(playlist->songs), is_shuffled ? (guint)g_random_int_range(0, (gint32)count) : 0,
			is_shuffled, window);
	else if (!playlist->has_songs) {
		window->waiting_play_id = g_strdup(playlist->id);
		window->is_waiting_play_shuffled = is_shuffled;
	}
}


void play_playlist_now(Window *window, CrimsonPlaylist *playlist, gboolean is_shuffled) {
	g_set_object(&window->menu_playlist, playlist);
	play_menu_playlist(window, is_shuffled);
}


static void play_playlist(GSimpleAction *action, GVariant *parameter, gpointer data) {
	play_menu_playlist(data, FALSE);
}


static void play_playlist_random(GSimpleAction *action, GVariant *parameter, gpointer data) {
	play_menu_playlist(data, TRUE);
}


/* Dialogs ================================================================= */


static void allow_response(GtkEditable *entry, AdwAlertDialog *dialog) {
	g_autofree char *text = g_strstrip(g_strdup(gtk_editable_get_text(entry)));
	adw_alert_dialog_set_response_enabled(dialog, "confirm", *text != 0);
}


/* A dialog with one name field, which runs done with the name once it is confirmed */
static void ask_name(Window *window, const char *heading, const char *name, const char *confirm,
	GAsyncReadyCallback done) {
	AdwDialog *dialog = adw_alert_dialog_new(heading, NULL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "confirm", confirm, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "confirm");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TEXT_PLAYLIST_NAME);
	gtk_editable_set_text(GTK_EDITABLE(entry), name);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	g_signal_connect(entry, "changed", G_CALLBACK(allow_response), dialog);
	allow_response(GTK_EDITABLE(entry), ADW_ALERT_DIALOG(dialog));
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
	g_object_set_data(G_OBJECT(dialog), "entry", entry);
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window->window), NULL, done, window);
}


static char *read_answer(GObject *dialog, GAsyncResult *result) {
	if (!g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "confirm"))
		return NULL;
	GtkEditable *entry = g_object_get_data(dialog, "entry");
	return g_strstrip(g_strdup(gtk_editable_get_text(entry)));
}


static void finish_new_playlist(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autofree char *name = read_answer(dialog, result);
	if (name && *name)
		create_playlist(((Window *)data)->engine, name);
}


/* A new local playlist goes first in the sidebar and opens at once */
static void finish_new_local_playlist(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	g_autofree char *name = read_answer(dialog, result);
	if (!name || !*name)
		return;
	g_autoptr(CrimsonPlaylist) playlist = create_local_playlist(name);
	describe_local_playlist(playlist);
	g_list_store_insert(window->playlists, 0, playlist);
	save_local_playlists(window->playlists);
	select_playlist_row(window, playlist->id);
}


static void finish_new_local_playlist_with_song(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	g_autoptr(CrimsonSong) song = window->menu_song ? g_object_ref(window->menu_song) : NULL;
	finish_new_local_playlist(dialog, result, data);
	g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), 0);
	if (song && playlist && is_local_playlist(playlist) && !g_list_model_get_n_items(G_LIST_MODEL(playlist->songs)))
		add_song_to(window, playlist, song);
}


static void ask_new_local_playlist_with_song(GSimpleAction *action, GVariant *parameter, gpointer data) {
	ask_name(data, TEXT_NEW_LOCAL_PLAYLIST, "", TEXT_CREATE, finish_new_local_playlist_with_song);
}


static void ask_new_local_playlist(GSimpleAction *action, GVariant *parameter, gpointer data) {
	ask_name(data, TEXT_NEW_LOCAL_PLAYLIST, "", TEXT_CREATE, finish_new_local_playlist);
}


/* Signed out, a new playlist is kept on this computer instead */
static void ask_new_playlist(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->is_signed_in)
		ask_name(data, TEXT_NEW_PLAYLIST, "", TEXT_CREATE, finish_new_playlist);
	else
		ask_new_local_playlist(action, parameter, data);
}


static void finish_rename(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	g_autofree char *name = read_answer(dialog, result);
	if (!name || !*name || !window->menu_playlist)
		return;
	if (!is_local_playlist(window->menu_playlist)) {
		rename_playlist(window->engine, window->menu_playlist->id, name);
		return;
	}
	g_set_str(&window->menu_playlist->title, name);
	save_playlist(window, window->menu_playlist);
	guint position;
	if (g_list_store_find(window->playlists, window->menu_playlist, &position))
		g_list_store_splice(window->playlists, position, 1, (gpointer *)&window->menu_playlist, 1);
	if (window->menu_playlist == window->shown)
		select_playlist_row(window, window->shown->id);
}


static void ask_rename(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->menu_playlist)
		ask_name(window, TEXT_RENAME_PLAYLIST, window->menu_playlist->title, TEXT_RENAME, finish_rename);
}


static void finish_delete(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (!g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "confirm"))
		return;
	if (!window->menu_playlist)
		return;
	if (!is_local_playlist(window->menu_playlist)) {
		delete_playlist(window->engine, window->menu_playlist->id);
		return;
	}
	guint position;
	if (g_list_store_find(window->playlists, window->menu_playlist, &position))
		g_list_store_remove(window->playlists, position);
	save_local_playlists(window->playlists);
	if (window->menu_playlist == window->shown)
		select_playlist_row(window, NULL);
}


static void ask_delete(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (!window->menu_playlist)
		return;
	g_autofree char *heading = g_strdup_printf(TEXT_DELETE_PLAYLIST, window->menu_playlist->title);
	AdwDialog *dialog = adw_alert_dialog_new(heading, TEXT_DELETE_PLAYLIST_DETAIL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "confirm", TEXT_DELETE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window->window), NULL, finish_delete, window);
}


/* Back takes the lyrics and the video away before it leaves the page under them */
static void go_back(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->is_lyrics_open || window->is_video_open) {
		close_song_views(window);
		return;
	}
	if (adw_navigation_view_pop(ADW_NAVIGATION_VIEW(window->navigation)) || !window->is_back_to_search)
		return;
	start_search(GTK_SEARCH_ENTRY(window->search_entry), window);
}


/* Opens the page last gone back from, fetching an artist or album again or repeating the search */
static void go_forward(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (!window->forward_tags->len)
		return;
	g_autofree char *tag = g_ptr_array_steal_index(window->forward_tags, window->forward_tags->len - 1);
	window->is_going_forward = TRUE;
	if (g_str_equal(tag, SEARCH_TAG))
		start_search(GTK_SEARCH_ENTRY(window->search_entry), window);
	else
		open_card(tag, window);
}


static void focus_search(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	gtk_widget_grab_focus(window->search_entry);
}


void add_page_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"play-next", play_next_song, NULL, NULL, NULL, {0}},
		{"needs-account", needs_account, "s", NULL, NULL, {0}},
		{"search-for", search_for, "s", NULL, NULL, {0}},
		{"open-location", open_location, "s", NULL, NULL, {0}},
		{"share", share_song, "s", NULL, NULL, {0}},
		{"local-version", NULL, "s", "''", pick_local_version, {0}},
		{"new-local-playlist", ask_new_local_playlist, NULL, NULL, NULL, {0}},
		{"new-local-playlist-with-song", ask_new_local_playlist_with_song, NULL, NULL, NULL, {0}},
		{"add-to-queue", add_to_queue, NULL, NULL, NULL, {0}},
		{"open-card", open_card_action, "s", NULL, NULL, {0}},
		{"find-album", find_album_action, "s", NULL, NULL, {0}},
		{"go-to-playlist", go_to_library_playlist, "s", NULL, NULL, {0}},
		{"unpin", unpin_action, "s", NULL, NULL, {0}},
		{"song-information", show_song_information, NULL, NULL, NULL, {0}},
		{"playing-information", show_playing_information, NULL, NULL, NULL, {0}},
		{"love-song", love_song_action, NULL, NULL, NULL, {0}},
		{"similar-songs", show_similar_songs, NULL, NULL, NULL, {0}},
		{"similar-artists", show_similar_artists, "s", NULL, NULL, {0}},
		{"new-dynamic", new_dynamic_action, NULL, NULL, NULL, {0}},
		{"edit-dynamic", edit_dynamic_action, "s", NULL, NULL, {0}},
		{"delete-dynamic", ask_dynamic_delete, NULL, NULL, NULL, {0}},
		{"add-to-playlist", add_song_to_playlist, "s", NULL, NULL, {0}},
		{"remove-from-playlist", remove_song, NULL, NULL, NULL, {0}},
		{"play-playlist", play_playlist, NULL, NULL, NULL, {0}},
		{"play-playlist-random", play_playlist_random, NULL, NULL, NULL, {0}},
		{"rename-playlist", ask_rename, NULL, NULL, NULL, {0}},
		{"delete-playlist", ask_delete, NULL, NULL, NULL, {0}},
		{"search", focus_search, NULL, NULL, NULL, {0}},
		{"back", go_back, NULL, NULL, NULL, {0}},
		{"forward", go_forward, NULL, NULL, NULL, {0}},
		{"new-playlist", ask_new_playlist, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(window->window), ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	window->version_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "local-version"));
}


/* Lyrics ================================================================== */


/* The page shows the words only once they belong to the song in the player */
static void fill_lyrics_page(Window *window) {
	CrimsonSong *song = get_queue_song(window->queue);
	gboolean is_current = song && g_strcmp0(window->lyrics_video, song->id) == 0;
	show_lyrics(window->lyrics_page, song ? song->title : NULL, song ? song->artist : NULL,
		is_current ? window->lyrics_text : NULL, is_current ? window->lyrics_source : NULL,
		is_current ? window->lyrics_lines : NULL);
	follow_lyrics(window->lyrics_page, window->time);
}


static void clear_lyrics(Window *window) {
	g_clear_pointer(&window->lyrics_video, g_free);
	g_clear_pointer(&window->lyrics_text, g_free);
	g_clear_pointer(&window->lyrics_source, g_free);
	g_clear_pointer(&window->lyrics_lines, g_ptr_array_unref);
}


/* A song without words takes the open lyrics away, and so does going anywhere else */
void close_lyrics(Window *window) {
	if (window->lyrics_action && window->is_lyrics_open)
		g_action_change_state(G_ACTION(window->lyrics_action), g_variant_new_boolean(FALSE));
}


void ask_for_lyrics(Window *window, CrimsonSong *song) {
	clear_lyrics(window);
	set_player_has_lyrics(window->bar, FALSE);
	if (is_local_song(song->id))
		close_lyrics(window);
	else
		request_lyrics(window->engine, song->id);
	fill_lyrics_page(window);
}


static GPtrArray *read_lyric_lines(JSCValue *message) {
	GPtrArray *lines = g_ptr_array_new_with_free_func((GDestroyNotify)free_lyric_line);
	g_autoptr(JSCValue) array = jsc_value_object_get_property(message, "lines");
	g_autoptr(JSCValue) length = jsc_value_object_get_property(array, "length");
	for (guint index = 0; jsc_value_is_array(array) && index < (guint)jsc_value_to_int32(length); index++) {
		g_autoptr(JSCValue) item = jsc_value_object_get_property_at_index(array, index);
		g_autoptr(JSCValue) start = jsc_value_object_get_property(item, "start");
		LyricLine *line = g_new0(LyricLine, 1);
		line->text = read_text(item, "text");
		line->start = (gint64)jsc_value_to_double(start);
		g_ptr_array_add(lines, line);
	}
	return lines;
}


static void receive_lyrics(Window *window, JSCValue *message) {
	g_autofree char *video = read_text(message, "video");
	if (g_strcmp0(video, window->current_video) != 0)
		return;
	clear_lyrics(window);
	window->lyrics_video = g_steal_pointer(&video);
	window->lyrics_text = read_text(message, "text");
	window->lyrics_source = read_text(message, "source");
	window->lyrics_lines = read_lyric_lines(message);
	gboolean has_words = *window->lyrics_text || window->lyrics_lines->len;
	set_player_has_lyrics(window->bar, has_words);
	if (!has_words)
		close_lyrics(window);
	fill_lyrics_page(window);
}


void follow_playing_lyrics(Window *window) {
	if (window->lyrics_page && window->is_lyrics_open)
		follow_lyrics(window->lyrics_page, window->time);
}


/* Comments ================================================================ */


static void close_comments(Window *window) {
	if (window->comments_action && gtk_revealer_get_reveal_child(GTK_REVEALER(window->comments_revealer)))
		g_action_change_state(G_ACTION(window->comments_action), g_variant_new_boolean(FALSE));
}


void ask_for_comments(Window *window, CrimsonSong *song) {
	g_set_str(&window->comments_video, song->id);
	g_clear_pointer(&window->comments_next, g_free);
	set_player_has_comments(window->bar, FALSE);
	clear_comments(window->comments_page);
	if (is_local_song(song->id))
		close_comments(window);
	else
		request_comments(window->engine, song->id, NULL);
}


static GPtrArray *read_comments(JSCValue *message) {
	GPtrArray *comments = g_ptr_array_new_with_free_func((GDestroyNotify)free_comment);
	g_autoptr(JSCValue) array = jsc_value_object_get_property(message, "comments");
	g_autoptr(JSCValue) length = jsc_value_object_get_property(array, "length");
	for (guint index = 0; jsc_value_is_array(array) && index < (guint)jsc_value_to_int32(length); index++) {
		g_autoptr(JSCValue) item = jsc_value_object_get_property_at_index(array, index);
		Comment *comment = g_new0(Comment, 1);
		*comment = (Comment){read_text(item, "author"), read_text(item, "text"), read_text(item, "published"),
			read_text(item, "likes"), read_text(item, "replies"), read_text(item, "repliesToken")};
		g_ptr_array_add(comments, comment);
	}
	return comments;
}


/* The first page of a song's comments decides whether its button shows, and later pages add to the list */
static void receive_comments(Window *window, JSCValue *message) {
	g_autofree char *video = read_text(message, "video");
	if (g_strcmp0(video, window->comments_video) != 0)
		return;
	gboolean is_first = window->comments_next == NULL;
	g_autoptr(GPtrArray) comments = read_comments(message);
	g_free(window->comments_next);
	window->comments_next = read_text(message, "next");
	add_comments(window->comments_page, comments, *window->comments_next != '\0');
	if (!is_first)
		return;
	set_player_has_comments(window->bar, comments->len > 0);
	if (!comments->len)
		close_comments(window);
}


static void receive_replies(Window *window, JSCValue *message) {
	g_autofree char *video = read_text(message, "video");
	if (g_strcmp0(video, window->comments_video) != 0)
		return;
	g_autoptr(JSCValue) id = jsc_value_object_get_property(message, "id");
	g_autoptr(GPtrArray) replies = read_comments(message);
	g_autofree char *next = read_text(message, "next");
	add_replies(window->comments_page, (guint)jsc_value_to_int32(id), replies, next);
}


static void ask_for_replies(const char *token, guint id, gpointer data) {
	Window *window = data;
	if (window->comments_video)
		request_replies(window->engine, window->comments_video, token, id);
}


static void ask_for_more_comments(gpointer data) {
	Window *window = data;
	if (window->comments_video && window->comments_next && *window->comments_next)
		request_comments(window->engine, window->comments_video, window->comments_next);
}


/* The Comments button slides the panel in from the right beside whatever the middle of the window shows */
void change_comments(GSimpleAction *action, GVariant *state, gpointer data) {
	Window *window = data;
	g_simple_action_set_state(action, state);
	gtk_revealer_set_reveal_child(GTK_REVEALER(window->comments_revealer), g_variant_get_boolean(state));
}


static GtkWidget *add_comments_panel(Window *window, GtkWidget *middle) {
	window->comments_page = create_comments_page(ask_for_more_comments, ask_for_replies, window);
	gtk_widget_set_size_request(window->comments_page, COMMENTS_WIDTH, -1);
	window->comments_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(window->comments_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
	gtk_revealer_set_transition_duration(GTK_REVEALER(window->comments_revealer), SONG_VIEW_SLIDE_MS);
	gtk_revealer_set_child(GTK_REVEALER(window->comments_revealer), window->comments_page);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(middle, TRUE);
	gtk_box_append(GTK_BOX(box), middle);
	gtk_box_append(GTK_BOX(box), window->comments_revealer);
	return box;
}


/* Song views ============================================================== */


/* Each view rises from below the middle of the window, the video taking the top half once the lyrics are up and the
 * lyrics the bottom half once the video is up, so opening one beside the other shares the space smoothly. A closed
 * video leaves the page that plays the music as the invisible speck in the corner it always was */
static gboolean place_song_view(GtkOverlay *overlay, GtkWidget *widget, GdkRectangle *place, Window *window) {
	int width = gtk_widget_get_width(GTK_WIDGET(overlay));
	double height = gtk_widget_get_height(GTK_WIDGET(overlay));
	if (widget == window->lyrics_page) {
		double lyrics_height = height * (1 - SHARED_SHARE * window->video_rise);
		*place = (GdkRectangle){0, (int)(height - window->lyrics_rise * lyrics_height), width, (int)lyrics_height};
		return TRUE;
	}
	if (widget != get_engine_widget(window->engine) && widget != window->video_cover && widget != window->video_backing)
		return FALSE;
	if (!window->is_video_open && window->video_rise <= 0)
		return FALSE;
	double video_height = height * (1 - SHARED_SHARE * window->lyrics_rise);
	*place = (GdkRectangle){0, (int)((1 - window->video_rise) * height), width, (int)video_height};
	return TRUE;
}


static void set_lyrics_rise(double value, gpointer data) {
	Window *window = data;
	window->lyrics_rise = value;
	gtk_widget_queue_allocate(window->center_overlay);
}


static void set_video_rise(double value, gpointer data) {
	Window *window = data;
	window->video_rise = value;
	gtk_widget_queue_allocate(window->center_overlay);
}


/* A view slid all the way down is hidden, so it takes no clicks and costs no drawing */
static void finish_lyrics_slide(Window *window) {
	gtk_widget_set_visible(window->lyrics_page, window->is_lyrics_open);
}


/* An open video fills its place, while a closed one goes back to an invisible speck. The page itself never takes
 * clicks, which a cover over it turns into the app's own play and pause, and the cover stays black until YouTube's
 * own pages are gone from it */
static void uncover_video(gpointer data) {
	Window *window = data;
	gtk_widget_remove_css_class(window->video_cover, "covering");
}


static void show_page(Window *window, gboolean is_shown) {
	GtkWidget *page = get_engine_widget(window->engine);
	gtk_widget_set_visible(window->video_cover, is_shown);
	gtk_widget_set_visible(window->video_backing, is_shown);
	gtk_widget_add_css_class(window->video_cover, "covering");
	gtk_widget_set_halign(page, is_shown ? GTK_ALIGN_FILL : GTK_ALIGN_START);
	gtk_widget_set_valign(page, is_shown ? GTK_ALIGN_FILL : GTK_ALIGN_START);
	show_engine_video(window->engine, is_shown, uncover_video);
}


static void finish_video_slide(Window *window) {
	if (!window->is_video_open)
		show_page(window, FALSE);
}


static void slide(AdwAnimation *animation, double from, gboolean is_open) {
	adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(animation), from);
	adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(animation), is_open ? 1 : 0);
	adw_animation_play(animation);
}


/* The Lyrics button slides the words up over the middle of the window, and Back slides them away again */
void change_lyrics(GSimpleAction *action, GVariant *state, gpointer data) {
	Window *window = data;
	gboolean is_open = g_variant_get_boolean(state);
	g_simple_action_set_state(action, state);
	if (is_open == window->is_lyrics_open)
		return;
	window->is_lyrics_open = is_open;
	if (is_open) {
		fill_lyrics_page(window);
		gtk_widget_set_visible(window->lyrics_page, TRUE);
	}
	slide(window->lyrics_slide, window->lyrics_rise, is_open);
	update_back_button(window);
}


/* The camera button slides the page that plays the music up into view, filled with its video */
void change_video(GSimpleAction *action, GVariant *state, gpointer data) {
	Window *window = data;
	gboolean is_open = g_variant_get_boolean(state);
	g_simple_action_set_state(action, state);
	if (is_open == window->is_video_open)
		return;
	window->is_video_open = is_open;
	if (is_open)
		show_page(window, TRUE);
	slide(window->video_slide, window->video_rise, is_open);
	update_back_button(window);
}


static void close_video(Window *window) {
	if (window->video_action && window->is_video_open)
		g_action_change_state(G_ACTION(window->video_action), g_variant_new_boolean(FALSE));
}


void close_song_views(Window *window) {
	close_lyrics(window);
	close_video(window);
}


void follow_music_video(Window *window, const PlayerReport *report) {
	gboolean has_video = report->is_video && !is_local_song(window->current_video);
	if (has_video != window->has_video) {
		window->has_video = has_video;
		set_player_has_video(window->bar, has_video);
		if (!has_video)
			close_video(window);
	}
	if (!has_video || !get_settings()->autoplays_videos || g_strcmp0(window->video_autoplayed, window->current_video) == 0)
		return;
	g_set_str(&window->video_autoplayed, window->current_video);
	if (!window->is_video_open)
		g_action_change_state(G_ACTION(window->video_action), g_variant_new_boolean(TRUE));
}


static void toggle_video_playing(Window *window) {
	g_action_group_activate_action(G_ACTION_GROUP(window->window), "play-pause", NULL);
}


static AdwAnimation *create_slide(Window *window, AdwAnimationTargetFunc set_rise, GCallback finish) {
	AdwAnimationTarget *target = adw_callback_animation_target_new(set_rise, window, NULL);
	AdwAnimation *animation = adw_timed_animation_new(window->center_overlay, 0, 1, SONG_VIEW_SLIDE_MS, target);
	adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(animation), ADW_EASE_OUT_CUBIC);
	g_signal_connect_swapped(animation, "done", finish, window);
	return animation;
}


/* The page that plays the music lives in the middle of the window from the start, since moving a web view
 * elsewhere could pause it */
static GtkWidget *add_song_views(Window *window, GtkWidget *navigation) {
	window->lyrics_page = create_lyrics_page(seek, window);
	gtk_widget_set_visible(window->lyrics_page, FALSE);
	window->center_overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(window->center_overlay), navigation);
	/* Black lies behind the video, so a page still catching up with its new size never shows the list beneath */
	window->video_backing = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(window->video_backing, "video-backing");
	gtk_widget_set_visible(window->video_backing, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(window->center_overlay), window->video_backing);
	gtk_overlay_add_overlay(GTK_OVERLAY(window->center_overlay), get_engine_widget(window->engine));
	gtk_overlay_set_clip_overlay(GTK_OVERLAY(window->center_overlay), get_engine_widget(window->engine), TRUE);
	window->video_cover = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(window->video_cover, "video-cover");
	gtk_widget_set_visible(window->video_cover, FALSE);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect_swapped(click, "released", G_CALLBACK(toggle_video_playing), window);
	gtk_widget_add_controller(window->video_cover, GTK_EVENT_CONTROLLER(click));
	gtk_overlay_add_overlay(GTK_OVERLAY(window->center_overlay), window->video_cover);
	gtk_overlay_add_overlay(GTK_OVERLAY(window->center_overlay), window->lyrics_page);
	gtk_overlay_set_clip_overlay(GTK_OVERLAY(window->center_overlay), window->lyrics_page, TRUE);
	g_signal_connect(window->center_overlay, "get-child-position", G_CALLBACK(place_song_view), window);
	window->lyrics_slide = create_slide(window, set_lyrics_rise, G_CALLBACK(finish_lyrics_slide));
	window->video_slide = create_slide(window, set_video_rise, G_CALLBACK(finish_video_slide));
	return window->center_overlay;
}


/* Navigation ============================================================== */


/* Any page over the playlist can go back, and so can a playlist opened from a search result */
void update_back_button(Window *window) {
	g_autoptr(GListModel) stack = adw_navigation_view_get_navigation_stack(ADW_NAVIGATION_VIEW(window->navigation));
	guint depth = g_list_model_get_n_items(stack);
	if (depth > 1)
		window->is_back_to_search = FALSE;
	gboolean is_view_open = window->is_lyrics_open || window->is_video_open;
	gtk_widget_set_visible(window->back_button, depth > 1 || window->is_back_to_search || is_view_open);
}


/* Any other page coming into view takes the lyrics and the video away, whether it was opened, gone back to or found
 * already open, as the player bar's links do */
static void notice_visible_page(AdwNavigationView *navigation, GParamSpec *spec, Window *window) {
	close_song_views(window);
	/* Going back from Home to the playlist under it lights that playlist's row again in place of Home's */
	AdwNavigationPage *visible = adw_navigation_view_get_visible_page(navigation);
	const char *tag = visible ? adw_navigation_page_get_tag(visible) : NULL;
	if (g_strcmp0(tag, PLAYLIST_TAG) == 0 && gtk_list_box_get_selected_row(GTK_LIST_BOX(window->home_list))) {
		gtk_list_box_unselect_all(GTK_LIST_BOX(window->home_list));
		select_playlist_row(window, window->state.playlist_id);
	}
	update_back_button(window);
}


static void remember_popped(AdwNavigationView *navigation, AdwNavigationPage *page, Window *window) {
	const char *tag = adw_navigation_page_get_tag(page);
	if (tag && !g_str_equal(tag, PLAYLIST_TAG))
		g_ptr_array_add(window->forward_tags, g_strdup(tag));
}


/* A page opened any way but Forward starts a new path, so what Forward would have reopened is forgotten */
static void forget_forward(AdwNavigationView *navigation, Window *window) {
	close_lyrics(window);
	if (!window->is_going_forward)
		g_ptr_array_set_size(window->forward_tags, 0);
	window->is_going_forward = FALSE;
}


/* The mouse's back and forward buttons work anywhere in the window */
static void press_side_button(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	if (button != BACK_BUTTON && button != FORWARD_BUTTON)
		return;
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	gtk_widget_activate_action(GTK_WIDGET(window->window), button == BACK_BUTTON ? "win.back" : "win.forward",
		NULL);
}


void add_side_buttons(Window *window) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
	g_signal_connect(click, "pressed", G_CALLBACK(press_side_button), window);
	gtk_widget_add_controller(GTK_WIDGET(window->window), GTK_EVENT_CONTROLLER(click));
}


GtkWidget *create_navigation(Window *window) {
	window->navigation = adw_navigation_view_new();
	window->playlist_page = create_song_page(&SONG_PAGE_EVENTS, window, TRUE);
	set_song_page_heading(window->playlist_page, NULL, NULL, NULL, FALSE);
	add_page_drop(window);
	AdwNavigationPage *root = adw_navigation_page_new_with_tag(get_song_page_widget(window->playlist_page),
		TEXT_APPLICATION_NAME, PLAYLIST_TAG);
	adw_navigation_view_add(ADW_NAVIGATION_VIEW(window->navigation), root);
	g_signal_connect(window->navigation, "notify::visible-page", G_CALLBACK(notice_visible_page), window);
	window->forward_tags = g_ptr_array_new_with_free_func(g_free);
	window->online_results = g_ptr_array_new();
	window->liked_videos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	window->suggestions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	g_signal_connect(window->navigation, "popped", G_CALLBACK(remember_popped), window);
	g_signal_connect(window->navigation, "pushed", G_CALLBACK(forget_forward), window);
	return add_comments_panel(window, add_song_views(window, window->navigation));
}


/* The title in the player bar leads back to the playlist, page or search the song plays from */
/* Goes to where the playing song plays from, scrolled to the song, which lights up for a moment */
void open_queue_source(gpointer data) {
	Window *window = data;
	/* The source may be the page already showing, which no page change would close the lyrics and video over */
	close_song_views(window);
	const char *tag = window->queue_page_tag;
	const char *video = window->current_video;
	if (!tag)
		return;
	if (g_str_equal(tag, PLAYLIST_TAG)) {
		select_playlist_row(window, window->queue_playlist_id);
		adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
		if (video)
			reveal_song_page_song(window->playlist_page, video);
	} else if (g_str_equal(tag, SEARCH_TAG)) {
		AdwNavigationPage *page = adw_navigation_view_find_page(ADW_NAVIGATION_VIEW(window->navigation), tag);
		if (page)
			adw_navigation_view_pop_to_page(ADW_NAVIGATION_VIEW(window->navigation), page);
		SongPage *songs = find_song_page(window, SEARCH_TAG);
		if (songs && video)
			reveal_song_page_song(songs, video);
	} else {
		SongPage *songs = find_song_page(window, tag);
		g_set_str(&window->reveal_video, songs ? NULL : video);
		open_card(tag, window);
		if (songs && video)
			reveal_song_page_song(songs, video);
	}
}


void open_playing_artist(const char *artist_id, gpointer data) {
	Window *window = data;
	close_song_views(window);
	CrimsonSong *song = window->current_video ? get_queue_song(window->queue) : NULL;
	if (!song)
		return;
	if (*artist_id) {
		open_card(artist_id, window);
		return;
	}
	/* A local file names its artist only as text, so the artist is searched for instead */
	if (is_local_song(song->id) && *song->artist) {
		g_autofree char *artist = g_strdup_printf("\"%s\"", song->artist);
		gtk_editable_set_text(GTK_EDITABLE(window->search_entry), artist);
		start_search(GTK_SEARCH_ENTRY(window->search_entry), window);
		return;
	}
	g_autofree char *first = get_song_artist_id(song);
	if (*first)
		open_card(first, window);
}


void open_playing_menu(GtkWidget *widget, double x, double y, gpointer data) {
	Window *window = data;
	CrimsonSong *song = window->current_video ? get_queue_song(window->queue) : NULL;
	if (song)
		open_song_menu(song, 0, widget, x, y, window);
}

#pragma once

#include "dynamic.h"
#include "engine.h"
#include "library.h"
#include "local-player.h"
#include "player-bar.h"
#include "queue.h"
#include "song-page.h"
#include "state.h"
#include "mpris.h"
#include "tray.h"
#include "window.h"

/* Navigation tags of the pages that are not an artist or album, which use their own ids */
#define PLAYLIST_TAG "playlist"
#define SEARCH_TAG "search"
/* YouTube Music's Home page, which opens from the top of the YouTube playlists */
#define HOME_TAG "home"
/* YouTube Music's own playlist of liked songs */
#define LIKED_PLAYLIST "LM"

/* Everything one window holds, shared by its library, pages and playback parts */
struct Window {
	AdwApplicationWindow *window;
	GtkWidget *lastfm_reminder, *lastfm_button, *lastfm_avatar;
	GtkWidget *toasts, *main_stack, *split, *sidebar_stack, *playlist_list, *pin_list, *pin_heading;
	/* Shown in place of the YouTube playlists while they load, or when the account has none */
	GtkWidget *playlists_spinner, *playlists_empty;
	GtkWidget *search_entry, *back_button, *top_spinner, *navigation, *player_revealer, *menu_anchor;
	SongPage *playlist_page;
	PlayerBar *bar;
	Tray *tray;
	Mpris *mpris;
	Engine *engine;
	GListStore *playlists;
	/* The YouTube playlists, and the media library followed by the playlists kept on this computer, as the sidebar
	 * lists them */
	GListModel *youtube_playlists, *local_section;
	/* The line above the YouTube playlists, shown only while there are any */
	GtkWidget *youtube_gap;
	CrimsonPlaylist *shown;
	Queue *queue;
	PlayerState state;
	char *current_video;
	double time;
	gboolean is_playing, is_signed_in;
	/* The YouTube Music page has said whether anyone is signed in */
	gboolean is_ready;
	/* The current song has really begun playing, so seeking it can take effect */
	gboolean has_started;
	/* The song is loading or buffering, which play and pause wait out */
	gboolean is_waiting;
	/* The song on show came back from the last time and has not been started yet */
	gboolean is_restored;
	/* How long the current song was really heard, which decides whether it counts as a listen on Last.fm */
	double heard_seconds, last_time;
	gint64 started_at;
	gboolean is_scrobbled;
	/* Playlists came from the cache or YouTube, so an empty sidebar really is empty */
	gboolean has_library;
	int search_token;
	/* The search page's row of matching songs from the listener's playlists, and its rows from YouTube */
	GtkWidget *library_results;
	/* A song from the listener's playlists was opened from search, so Back returns to that search */
	gboolean is_back_to_search;
	GPtrArray *online_results;
	/* The artist or album being fetched before its page slides in */
	char *waiting_card_id;
	/* Tags of the pages gone back from, newest last, which Forward opens again as a browser does */
	GPtrArray *forward_tags;
	/* Ids of the songs in Liked Music, changed at once on a like and set right by the next refresh */
	GHashTable *liked_videos;
	/* Songs the account played lately, newest first, as Home last listed them, and the page that lists them all */
	GListStore *history;
	char *history_id;
	/* Home as YouTube last sent it, shown at once the next time Home opens */
	Home *home;
	/* Songs YouTube Music suggests for each of the listener's own playlists, by playlist id, asked once a session */
	GHashTable *suggestions;
	/* Artists and albums pinned to the sidebar, as cards */
	GPtrArray *pins;
	/* Dynamic playlists, as the filters kept on this computer and the playlists of liked songs they pick */
	GPtrArray *dynamic_rules;
	GListStore *dynamic_playlists;
	GtkWidget *dynamic_list, *dynamic_gap;
	/* How far each playlist was scrolled, by playlist id */
	GKeyFile *scroll_places;
	/* The songs dynamic playlists pick from, filled again before each pick */
	GListStore *dynamic_pool;
	/* The music on this computer, as a playlist of its own with its own row and player */
	CrimsonPlaylist *local_playlist;
	GtkWidget *local_list, *local_cover;
	LocalPlayer *local_player;
	guint refilter_source;
	char *menu_dynamic_id;
	/* A playlist asked to play before its songs were known */
	char *waiting_play_id;
	/* The playing song to show once the page it plays from has loaded */
	char *reveal_video;
	/* The similar songs or artists page waiting for its answer, and its token, so a late answer finds no page */
	char *similar_tag;
	int similar_token;
	gboolean is_waiting_play_shuffled;
	gboolean is_going_forward;
	/* Where the queue was started, as a page tag, with the playlist when that page was a playlist */
	char *queue_page_tag, *queue_playlist_id;
	/* A playlist just made, selected once the library lists it */
	char *new_playlist_id;
	/* The song or playlist a right click menu is open for */
	CrimsonSong *menu_song;
	CrimsonPlaylist *menu_playlist;
	GSimpleAction *shuffle_action, *sign_out_action, *sign_in_action, *lyrics_action, *video_action, *comments_action;
	/* The playing song's words once YouTube answered, and the page that shows them while it is open */
	char *lyrics_video, *lyrics_text, *lyrics_source;
	GPtrArray *lyrics_lines;
	GtkWidget *lyrics_page;
	/* The lyrics and the music video rise from below the middle of the window, each at this share of the way up,
	 * and share it half and half while both are open */
	GtkWidget *center_overlay, *video_cover, *video_backing;
	/* The row at the top of the YouTube playlists that opens YouTube Music's Home */
	GtkWidget *home_list, *home_cover;
	AdwAnimation *lyrics_slide, *video_slide;
	double lyrics_rise, video_rise;
	gboolean is_lyrics_open, is_video_open, has_video;
	/* The comments panel at the right, the song they belong to and where YouTube's next comments start */
	GtkWidget *comments_page, *comments_revealer;
	char *comments_video, *comments_next;
	/* The song a music video last opened by itself for, so closing it keeps it closed for that song */
	char *video_autoplayed;
	/* Its state is the version of the local song whose menu is open, which marks that version in the menu */
	GSimpleAction *version_action;
	/* The card asking to sign in, shown in the sidebar while signed out */
	GtkWidget *sign_in_card;
};

/* Library, in window.c */
CrimsonPlaylist *find_playlist(Window *window, const char *playlist_id);
void show_message(Window *window, const char *text);
void show_playlist(Window *window, CrimsonPlaylist *playlist);
void select_playlist_row(Window *window, const char *playlist_id);
void refresh_playlist(Window *window, CrimsonPlaylist *playlist);
void update_playlist_page(Window *window);
void update_top_title(Window *window);
GtkWidget *create_status(const char *icon, const char *title, const char *detail);
/* TRUE when signed in, or else tells the listener the feature needs a YouTube Music account and offers to sign in */
gboolean require_account(Window *window, const char *feature);
GtkWidget *create_sign_in_card(void);
GtkWidget *create_scrolled(GtkWidget *child);
GtkWidget *create_stack(void);

/* Playlist sidebar, in window-sidebar.c */
GtkWidget *create_sidebar(Window *window);
void pin_card(Window *window, const Card *card);
void unpin_card(Window *window, const char *id);

/* Pages and editing, in window-pages.c */
extern const SongPageEvents SONG_PAGE_EVENTS;
void add_page_actions(Window *window);
GtkWidget *create_navigation(Window *window);
void add_side_buttons(Window *window);
/* Reads which songs are liked from Liked Music and shows it on every heart */
void update_liked(Window *window);
void toggle_playing_like(gpointer data);
void toggle_playing_dislike(gpointer data);
void toggle_playing_love(gpointer data);
void queue_dropped_song(CrimsonSong *song, gpointer data);
/* Opens the playlist and plays it, as soon as its songs are known when they are not yet */
void play_playlist_now(Window *window, CrimsonPlaylist *playlist, gboolean is_shuffled);
/* Shows whether the playing song is loved on Last.fm, with the heart hidden while it is not connected */
void show_playing_love(Window *window);
void open_card(const char *id, gpointer data);
void stop_waiting(Window *window);
void start_search(GtkSearchEntry *entry, Window *window);
void update_back_button(Window *window);
void show_youtube_gap(Window *window);
void open_song_menu(CrimsonSong *song, guint position, GtkWidget *row, double x, double y, gpointer data);
void open_queue_source(gpointer data);
void open_playing_artist(const char *artist_id, gpointer data);
void open_playing_menu(GtkWidget *widget, double x, double y, gpointer data);
void receive_answer(const char *type, JSCValue *message, gpointer data);
void open_playlist_menu(Window *window, CrimsonPlaylist *playlist, GtkWidget *row, double x, double y);
/* The menu for the sidebar around the playlists, which makes a new one */
void open_sidebar_menu(Window *window, GtkWidget *widget, double x, double y);
void open_pin_menu(Window *window, const char *id, GtkWidget *widget, double x, double y);
void open_dynamic_menu(Window *window, const char *id, GtkWidget *widget, double x, double y);

/* Dynamic playlists, in window-dynamic.c */
GtkWidget *create_dynamic_section(Window *window);
GtkWidget *create_local_section(Window *window);
/* A sidebar cover with a small icon in its corner, which tells a kind of playlist apart from the others */
GtkWidget *add_cover_badge(GtkWidget *cover, const char *icon);
/* A line fading out at both ends, which sets one sidebar section apart from the next */
GtkWidget *create_section_gap(void);
void describe_local_playlist(CrimsonPlaylist *playlist);
void save_playlist(Window *window, CrimsonPlaylist *playlist);
void select_local_row(Window *window);
CrimsonPlaylist *find_dynamic_playlist(Window *window, const char *id);
void select_dynamic_row(Window *window, const char *id);
/* Filters the liked songs again soon, after the likes, the gathered information or the listener's tags changed */
void update_dynamic_playlists(Window *window);
void edit_dynamic_playlist(Window *window, const char *id);
void delete_dynamic_playlist(Window *window, const char *id);
/* Adds the song to the playlist, which for Liked Music means liking it */
void add_song_to(Window *window, CrimsonPlaylist *playlist, CrimsonSong *song);
GtkWidget *create_menu_anchor(Window *window);

/* Playback, in window-player.c */
void play_songs(GListModel *songs, guint start, gboolean is_shuffled, gpointer data);
void choose_song(GListModel *songs, guint position, gpointer data);
/* Clears the last song's words and asks YouTube for the new song's */
void ask_for_lyrics(Window *window, CrimsonSong *song);
void change_lyrics(GSimpleAction *action, GVariant *state, gpointer data);
/* Lights the line being sung while the lyrics are open */
void follow_playing_lyrics(Window *window);
void close_lyrics(Window *window);
/* Takes the lyrics and the video away, as going anywhere else in the window does */
void close_song_views(Window *window);
void change_comments(GSimpleAction *action, GVariant *state, gpointer data);
/* Starts loading the comments of a new song, which shows the Comments button once there are any */
void ask_for_comments(Window *window, CrimsonSong *song);
void change_video(GSimpleAction *action, GVariant *state, gpointer data);
/* Shows the camera button for a music video, and opens the video when the listener asked for that */
void follow_music_video(Window *window, const PlayerReport *report);
void receive_player(const PlayerReport *report, gpointer data);
void toggle_playing(GSimpleAction *action, GVariant *parameter, gpointer data);
void play_next(GSimpleAction *action, GVariant *parameter, gpointer data);
void play_previous(GSimpleAction *action, GVariant *parameter, gpointer data);
void notice_song_info(const char *video_id, gpointer data);
void jump_queue(GSimpleAction *action, GVariant *parameter, gpointer data);
void remove_queued_song(GSimpleAction *action, GVariant *parameter, gpointer data);
void move_queued_song(GSimpleAction *action, GVariant *parameter, gpointer data);
/* Shows the songs coming up beside the Next button, after the queue changed */
void show_upcoming(Window *window);
/* Shows the suggestions for the playlist on screen, asking YouTube Music for them the first time */
void show_suggestions(Window *window);
/* Brings back the song the listener had up and their queue, paused, when Settings asks for it */
void restore_last_song(Window *window);
void jump_back(GSimpleAction *action, GVariant *parameter, gpointer data);
void jump_forward(GSimpleAction *action, GVariant *parameter, gpointer data);
void change_shuffle(GSimpleAction *action, GVariant *state, gpointer data);
void seek(double seconds, gpointer data);
void change_volume(int volume, gpointer data);

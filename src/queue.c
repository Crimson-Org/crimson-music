#include "queue.h"


Queue *create_queue(void) {
	Queue *queue = g_new0(Queue, 1);
	queue->songs = g_ptr_array_new_with_free_func(g_object_unref);
	queue->order = g_array_new(FALSE, FALSE, sizeof(guint));
	queue->queued = g_array_new(FALSE, TRUE, sizeof(gboolean));
	return queue;
}


/* Puts the playing song first when shuffling and keeps it current either way, so switching never skips */
static void order_queue(Queue *queue, guint current) {
	g_array_set_size(queue->order, 0);
	g_array_set_size(queue->queued, 0);
	if (!queue->is_shuffled) {
		for (guint index = 0; index < queue->songs->len; index++)
			g_array_append_val(queue->order, index);
		queue->position = current;
		g_array_set_size(queue->queued, queue->order->len);
		return;
	}
	g_array_append_val(queue->order, current);
	for (guint index = 0; index < queue->songs->len; index++)
		if (index != current)
			g_array_append_val(queue->order, index);
	for (guint index = queue->order->len - 1; index > 1; index--) {
		guint other = (guint)g_random_int_range(1, (gint32)index + 1);
		guint swapped = g_array_index(queue->order, guint, index);
		g_array_index(queue->order, guint, index) = g_array_index(queue->order, guint, other);
		g_array_index(queue->order, guint, other) = swapped;
	}
	queue->position = 0;
	g_array_set_size(queue->queued, queue->order->len);
}


void fill_queue(Queue *queue, GListModel *songs, guint start, gboolean is_shuffled) {
	g_ptr_array_set_size(queue->songs, 0);
	for (guint index = 0; index < g_list_model_get_n_items(songs); index++)
		g_ptr_array_add(queue->songs, g_list_model_get_item(songs, index));
	queue->is_shuffled = is_shuffled;
	if (queue->songs->len)
		order_queue(queue, MIN(start, queue->songs->len - 1));
	g_array_set_size(queue->queued, queue->order->len);
}


void clear_queue(Queue *queue) {
	g_ptr_array_set_size(queue->songs, 0);
	g_array_set_size(queue->order, 0);
	g_array_set_size(queue->queued, 0);
	queue->position = 0;
}


static void insert_place(Queue *queue, guint place, guint index, gboolean is_queued) {
	g_array_insert_val(queue->order, place, index);
	g_array_insert_val(queue->queued, place, is_queued);
}


/* The songs queued with Play Next stay right after the playing one, in their order, whatever the new order */
void shuffle_queue(Queue *queue, gboolean is_shuffled) {
	queue->is_shuffled = is_shuffled;
	if (!queue->order->len)
		return;
	g_autoptr(GArray) waiting = g_array_new(FALSE, FALSE, sizeof(guint));
	for (guint place = queue->position + 1; place < queue->order->len; place++) {
		if (g_array_index(queue->queued, gboolean, place))
			g_array_append_val(waiting, g_array_index(queue->order, guint, place));
	}
	order_queue(queue, g_array_index(queue->order, guint, queue->position));
	g_array_set_size(queue->queued, queue->order->len);
	for (guint index = 0; index < waiting->len; index++) {
		guint song = g_array_index(waiting, guint, index);
		for (guint place = 0; place < queue->order->len; place++) {
			if (g_array_index(queue->order, guint, place) == song && place != queue->position) {
				g_array_remove_index(queue->order, place);
				g_array_remove_index(queue->queued, place);
				if (place < queue->position)
					queue->position--;
				break;
			}
		}
		insert_place(queue, queue->position + 1 + index, song, TRUE);
	}
}


CrimsonSong *get_queue_song(Queue *queue) {
	if (queue->position >= queue->order->len)
		return NULL;
	return g_ptr_array_index(queue->songs, g_array_index(queue->order, guint, queue->position));
}


CrimsonSong *step_queue(Queue *queue, int step) {
	gint64 next = (gint64)queue->position + step;
	if (next < 0 || next >= (gint64)queue->order->len)
		return NULL;
	queue->position = (guint)next;
	return get_queue_song(queue);
}


void queue_song_next(Queue *queue, CrimsonSong *song, gboolean is_last) {
	guint index = queue->songs->len;
	g_ptr_array_add(queue->songs, g_object_ref(song));
	if (!queue->order->len) {
		insert_place(queue, 0, index, FALSE);
		return;
	}
	guint after = queue->position + 1;
	while (is_last && after < queue->order->len && g_array_index(queue->queued, gboolean, after))
		after++;
	insert_place(queue, after, index, TRUE);
}


GPtrArray *list_upcoming(Queue *queue, guint limit) {
	GPtrArray *upcoming = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint position = queue->position + 1; position < queue->order->len && upcoming->len < limit; position++)
		g_ptr_array_add(upcoming, g_object_ref(g_ptr_array_index(queue->songs, g_array_index(queue->order, guint,
			position))));
	return upcoming;
}


gboolean is_upcoming_queued(Queue *queue, guint offset) {
	guint place = queue->position + offset;
	return place < queue->queued->len && g_array_index(queue->queued, gboolean, place);
}


void remove_upcoming(Queue *queue, guint offset) {
	guint place = queue->position + offset;
	if (!offset || place >= queue->order->len)
		return;
	g_array_remove_index(queue->order, place);
	g_array_remove_index(queue->queued, place);
}


void move_upcoming(Queue *queue, guint from, guint to) {
	guint start = queue->position + from, end = queue->position + to;
	if (!from || !to || from == to || start >= queue->order->len || end >= queue->order->len)
		return;
	guint song = g_array_index(queue->order, guint, start);
	gboolean is_queued = g_array_index(queue->queued, gboolean, start);
	g_array_remove_index(queue->order, start);
	g_array_remove_index(queue->queued, start);
	insert_place(queue, end, song, is_queued);
}


void mark_upcoming_queued(Queue *queue, guint count) {
	for (guint place = queue->position + 1; place <= queue->position + count && place < queue->queued->len; place++)
		g_array_index(queue->queued, gboolean, place) = TRUE;
}

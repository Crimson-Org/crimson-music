#include "lang/en.h"
#include "app/sign-in.h"
#include "common/widgets/loading.h"
#include "app/dialog-layout.h"

#define SIGN_IN_ADDRESS "https://accounts.google.com/ServiceLogin?service=youtube&continue=" \
	"https%3A%2F%2Fmusic.youtube.com%2F"
#define FINISHED_ADDRESS "https://music.youtube.com/"
/* Google's page fits whole in this size, and the dialog is smaller only in a smaller window */
#define DIALOG_WIDTH 480
#define DIALOG_HEIGHT 680

/* The dialog and who to tell when the sign in is through */
typedef struct {
	AdwDialog *dialog;
	GDestroyNotify done;
	gpointer data;
	gboolean is_finished;
} SignIn;


static void notice_page(WebKitWebView *view, WebKitLoadEvent event, SignIn *sign_in) {
	if (event != WEBKIT_LOAD_COMMITTED || sign_in->is_finished
		|| !g_str_has_prefix(webkit_web_view_get_uri(view), FINISHED_ADDRESS))
		return;
	sign_in->is_finished = TRUE;
	webkit_web_view_stop_loading(view);
	sign_in->done(sign_in->data);
	adw_dialog_force_close(sign_in->dialog);
}


/* Google's page stays hidden behind the waiting spinner until its first load is through, then fades in, so the
 * dialog never shows a blank page */
static void show_first_page(WebKitWebView *view, WebKitLoadEvent event, GtkWidget *spinner) {
	if (event != WEBKIT_LOAD_FINISHED || !gtk_widget_get_visible(spinner))
		return;
	gtk_widget_set_visible(spinner, FALSE);
	fade_in_widget(GTK_WIDGET(view));
}


void show_sign_in(GtkWidget *parent, WebKitNetworkSession *session, GDestroyNotify done, gpointer data) {
	SignIn *sign_in = g_new0(SignIn, 1);
	*sign_in = (SignIn){adw_dialog_new(), done, data, FALSE};
	adw_dialog_set_title(sign_in->dialog, TEXT_SIGN_IN);
	adw_dialog_set_content_width(sign_in->dialog, DIALOG_WIDTH);
	fit_dialog_height(sign_in->dialog, parent, DIALOG_HEIGHT);
	GtkWidget *view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL);
	gtk_widget_set_vexpand(view, TRUE);
	gtk_widget_set_opacity(view, 0);
	g_signal_connect(view, "load-changed", G_CALLBACK(notice_page), sign_in);
	GtkWidget *spinner = create_loading_spinner();
	g_signal_connect(view, "load-changed", G_CALLBACK(show_first_page), spinner);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), view);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), spinner);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
	adw_dialog_set_child(sign_in->dialog, toolbar);
	g_object_set_data_full(G_OBJECT(sign_in->dialog), "sign-in", sign_in, g_free);
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), SIGN_IN_ADDRESS);
	adw_dialog_present(sign_in->dialog, parent);
}

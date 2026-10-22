#pragma once

#include <adwaita.h>
#include <webkit/webkit.h>

/* Shows Google's own sign in page in a dialog and calls done once it hands over to YouTube Music */
void show_sign_in(GtkWidget *parent, WebKitNetworkSession *session, GDestroyNotify done, gpointer data);

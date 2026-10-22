#pragma once

#include <webkit/webkit.h>

/* Copies the YouTube and Google cookies of a signed in Pear Desktop into the cookie store, then calls done,
 * which also happens at once when there is nothing to copy */
void import_pear_session(WebKitCookieManager *cookies, GDestroyNotify done, gpointer data);

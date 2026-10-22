#!/bin/sh
# Writes the Last.fm API key and shared secret into build/lastfm-keys.h, from CRIMSON_LASTFM_KEY and
# CRIMSON_LASTFM_SECRET or else from the first two lines of ~/.config/crimson-music-build/lastfm.ini, so they
# never sit in the repository. Without either, Last.fm stays switched off in the build.
set -eu
file="${HOME}/.config/crimson-music-build/lastfm.ini"
key="${CRIMSON_LASTFM_KEY:-}"
secret="${CRIMSON_LASTFM_SECRET:-}"
if [ -z "$key" ] && [ -r "$file" ]; then
	key=$(sed -n 1p "$file" | tr -cd '0-9a-fA-F')
	secret=$(sed -n 2p "$file" | tr -cd '0-9a-fA-F')
fi
header="build/lastfm-keys.h"
content=$(printf '#define LASTFM_KEY "%s"\n#define LASTFM_SECRET "%s"\n' "$key" "$secret")
# The header is only rewritten when it changes, so an unchanged build compiles nothing again
if [ ! -f "$header" ] || [ "$(cat "$header")" != "$content" ]; then
	printf '%s\n' "$content" > "$header"
fi

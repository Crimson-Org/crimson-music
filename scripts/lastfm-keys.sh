#!/bin/sh
# Writes the Last.fm API key and shared secret into build/lastfm-keys.h, from CRIMSON_LASTFM_KEY and
# CRIMSON_LASTFM_SECRET or else from the first two lines of ~/.config/crimson-music-build/lastfm.ini, so they
# never sit in the repository. Without either, Last.fm stays switched off in the build.
set -eu
file="${HOME}/.config/crimson-music-build/lastfm.ini"
key="${CRIMSON_LASTFM_KEY:-}"
secret="${CRIMSON_LASTFM_SECRET:-}"
if [ -z "$key" ] && [ -r "$file" ]; then
	key=$(sed -n 1p "$file")
	secret=$(sed -n 2p "$file")
fi
# Both are hexadecimal, so anything else is left out wherever they came from, and nothing can break the header
key=$(printf '%s' "$key" | tr -cd '0-9a-fA-F')
secret=$(printf '%s' "$secret" | tr -cd '0-9a-fA-F')
header="build/lastfm-keys.h"
content=$(printf '#define LASTFM_KEY "%s"\n#define LASTFM_SECRET "%s"\n' "$key" "$secret")
# The header is only rewritten when it changes, so an unchanged build compiles nothing again
if [ ! -f "$header" ] || [ "$(cat "$header")" != "$content" ]; then
	printf '%s\n' "$content" > "$header"
fi

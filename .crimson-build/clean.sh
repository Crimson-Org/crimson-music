#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Remove the build folder
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_remove() {
	rm -rf "$1"
}

export PROGRAM="crimson-music"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonMusic"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="c11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export PKGS="gtk4 libadwaita-1 webkitgtk-6.0 libsoup-3.0 sqlite3 gstreamer-1.0 gstreamer-pbutils-1.0"
export LIBS="-lm"
export DEFINES="-DAPPLICATION_VERSION=\"${VERSION}\""
export INCLUDES="-Isrc -Ibuild -Isubprojects/crimson-core/src"
printf '==> %s\n' 'remove build'
crimson_remove "build"

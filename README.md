<p align="center">
  <img src="./data/icons/scalable/apps/io.github.CrimsonOrg.CrimsonMusic.svg" alt="Crimson Music logo" width="128" height="128">
</p>

<h1 align="center">Crimson Music</h1>
<p align="center">A native <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">libadwaita</a> music player for <a href="https://music.youtube.com">YouTube Music</a>.</p>

<p align="center">
  <a href="https://gtk.org">
    <img src="https://img.shields.io/badge/Toolkit-GTK4-4a86cf?logo=gtk&logoColor=white" alt="GTK4">
  </a>
  <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">
    <img src="https://img.shields.io/badge/Style-libadwaita-3584e4?logo=gnome&logoColor=white" alt="libadwaita">
  </a>
  <a href="https://wayland.freedesktop.org/">
    <img src="https://img.shields.io/badge/Session-Wayland-ffbc00?logo=wayland&logoColor=white" alt="Wayland">
  </a>
</p>

## Why

The YouTube Music website fills the screen with feeds, shelves and recommendations.\
Crimson Music shows your playlists in a sidebar and the songs of the one you pick, and nothing else.

## Features

### Your library

Liked Music and every playlist you made or saved in YouTube Music, with their covers.\
The library from your last visit shows the instant the window opens and updates quietly behind it.\
Window size, the open playlist, volume and shuffle are kept between launches.

### Search, artists and albums

The search field in the middle of the top bar finds songs, artists and albums as you type, and Ctrl+F jumps to it.\
Matching songs from your own playlists come first, and a click plays the song in its playlist.\
Artist pages show the albums, singles, top songs and videos, and album pages list every track.\
Videos play as music, with only their sound.\
A right click on a song opens its artist or album, queues it next or adds it to a playlist.\
The heart beside a song, or in the player bar, adds it to Liked Music or takes it out.

### Your playlists

Ctrl+N or New Playlist in the menu makes one in your YouTube Music library, and a right click renames or deletes it.\
Songs in your own playlists move by dragging and leave with Remove from Playlist.\
Drag playlists in the sidebar to put them in your own order.

### Playing

A click on a song plays it and carries on through the playlist, in order or shuffled.\
Ctrl+P plays and pauses, Ctrl+Left and Ctrl+Right skip, and Ctrl+S turns shuffle on and off.\
In the player bar the title leads back to where the song plays from, the artist opens the artist and a right click opens the song menu.\
The player bar takes on the colours of the playing cover, which drift slowly behind the controls under a fine film grain.\
Settings, under Ctrl+comma, choose whether long songs resume where you left them and whether Crimson Music follows the system style or stays light or dark.

### Tray

The tray icon shows or hides the window, and its menu plays, pauses, skips and quits.\
The minimize button sends the window to the tray.

### Signing in

On the first launch Crimson Music signs in with your [Pear Desktop](https://github.com/pear-devs/pear-desktop) account when it finds one.\
Otherwise Google's own sign in page opens once.

### Fewer ads and trackers

Ad entries are removed from YouTube's player data before the player reads them.\
Google's ad and analytics servers are blocked.

## Building

You need a [C compiler](https://gcc.gnu.org/), [make](https://www.gnu.org/software/make/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org) 4.14, [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) 1.8, [WebKitGTK](https://webkitgtk.org) 6.0, [libsoup](https://libsoup.gnome.org) 3 and [SQLite](https://sqlite.org).\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel webkitgtk4-devel libsoup-devel sqlite3-devel`.

Clone it with `git clone --recurse-submodules` so the shared Crimson Core code comes along.\
`sh .crimson-build/install.sh` installs it and `sh .crimson-build/uninstall.sh` removes it.\
`sh .crimson-build/run.sh` builds and starts it and `sh .crimson-build/archive.sh` writes a tar.gz to `dist/`.\
`sh .crimson-build/appimage.sh` writes an [AppImage](https://appimage.org/) to `dist/` on [Arch Linux](https://archlinux.org/).

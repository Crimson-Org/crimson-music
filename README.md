<p align="center">
  <img src="./assets/icons/scalable/apps/io.github.CrimsonOrg.CrimsonMusic.svg" alt="Crimson Music logo" width="128" height="128">
</p>

<h1 align="center">Crimson Music</h1>
<p align="center">A native <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">libadwaita</a> music player for <a href="https://music.youtube.com">YouTube Music</a> on Linux.</p>

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

<p align="center">
  <a href="https://github.com/Crimson-Org/crimson-music/releases/latest"><b>Download Here!</b></a>
</p>

<p align="center">
  <img src="./demo1.png" alt="Crimson Music playing a song from an artist page">
</p>

## Why

The YouTube Music website fills the screen with feeds, shelves and recommendations.\
Crimson Music shows your playlists in a sidebar and the songs of the one you pick, and keeps recommendations to the Home page.

## About

### Your library

Your liked songs and playlists, with their covers, appear when the window opens.

### Search

Search your library and YouTube Music as you type, with your own playlists first.\
Artist and album pages show all their releases.

### Playlists

Create, rename and delete playlists, and drag songs and playlists to reorder them.

### Playing

Play playlists in order or shuffled, while the player bar takes on the colours of the cover.

### No account needed

Search and play without signing in, and sign in to add your playlists and liked songs.

### Your own music

The Local Media Library plays music from your Music folder, and Settings lets you pick other folders.

### Fewer ads

Ads are removed, and Google's ad and tracking servers are blocked.

## Building

You need a [C compiler](https://gcc.gnu.org/), [make](https://www.gnu.org/software/make/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org), [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/), [WebKitGTK](https://webkitgtk.org), [libsoup](https://libsoup.gnome.org), [SQLite](https://sqlite.org) and [GStreamer](https://gstreamer.freedesktop.org).\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel webkitgtk4-devel libsoup-devel sqlite3-devel gstreamer-devel gstreamer-plugins-base-devel`.

Steps:
 1. Clone using `git clone --recurse-submodules https://github.com/Crimson-Org/crimson-music`
 2. Run `sh .crimson-build/install.sh`

`sh .crimson-build/uninstall.sh` uninstalls Crimson Music.

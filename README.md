# Menulody

Menulody is a background music pak for NextUI. It plays MP3 and WAV files while you browse the menu, pauses when you launch games or other paks, and resumes when you return.

## Install

1. Copy `Menulody.pak` to `Tools/<platform>/` on your SD card.
2. Launch Menulody once from the Tools menu.
3. Add music to your library folder, or point Menulody at additional folders from Settings.
4. Optional: install and enable `Varnish.pak` if you want the now-playing overlay pill.

Supported platforms:

- `tg5040`
- `tg5050`
- `my355`

Supported audio formats:

- `.mp3`
- `.wav`

## How Playback Works

Menulody now has two playback modes:

- `Single Song Loop`: choose a song in `Choose Song` and press `A`. Menulody switches to that one track and loops it until you later choose a playlist source.
- `Playlist Source`: choose `All Songs` or a saved playlist in `Choose Playlist`. Menulody then plays from that source using the shuffle and repeat settings from `Details`.

Notes:

- Saved playlists are source selectors. They are not separate in-app music players anymore.
- Menulody remembers the selected source and the `Menu Music` on/off state across device reboots.
- `Preview` is temporary. It does not change the active source.
- If a preview is already playing, pressing `Y` on the same track stops it.
- Pressing `Y` on a different track switches the preview to that track.
- `Preview` is independent from the normal `Pause on Pak Launch` setting.

## Controls

Main screens follow Apostrophe / NextUI footer conventions:

- `B`: back or cancel
- `A`: confirm / select
- `START`: save on screens that stage changes first

Important screens:

- `Main Menu`
  - `Menu Music`: turn menu playback on or off
  - `Details`: open playback info and quick controls
- `Choose Song`
  - `A`: start single-song loop mode for the selected track
  - `Y`: preview / stop preview
- `Details`
  - `A`: act on the focused row
  - `L2`: previous track when a playlist source is active
  - `R2`: next track when a playlist source is active
  - `Shuffle`, `Repeat`, and `Volume` apply immediately
- `Choose Playlist`
  - `A`: activate `All Songs` or the selected saved playlist
  - `X`: delete saved playlist
  - `Y`: edit the selected saved playlist
  - `+ Create New Playlist`: create a saved playlist at the end of the list
- `Settings`
  - changes are staged locally
  - `START`: save
  - `B`: discard staged changes

## Settings

Settings are stored in shared userdata:

- `.../.userdata/shared/Menulody/settings.json`

You can configure:

- music folders
- pause on pak launch
- auto-start daemon at boot
- now-playing overlay duration via `Varnish.pak`

Playback details and quick controls live in `Details`:

- current state
- current source
- current track
- shuffle
- repeat
- volume

Music folders are scanned recursively.

Default music folder:

- `/mnt/SDCARD/Music`

Saved playlists are stored here:

- `.../.userdata/shared/Menulody/playlists/`

Remembered playback source state is stored here:

- `.../.userdata/shared/Menulody/source_state.json`

## Overlay

The now-playing pill appears:

- on first playback after boot
- when the actual song changes

It does not reappear when the same song loops in repeat-one mode.

This overlay is provided through `Varnish.pak`. Menulody playback still works without Varnish, but the pill will only appear when Varnish is fully enabled. Menulody checks the pak, enabled marker, startup patch, boot hook, and daemon state when showing warnings in `Settings`.

## Auto Pause / Resume

When Menulody is installed and run once, it installs launch hooks in userdata so it can:

- pause before ROM launches
- optionally pause before pak launches
- resume when you return to NextUI

If `Auto-Start Daemon` is enabled, Menulody also installs a boot hook so the daemon starts automatically.

## Troubleshooting

If no songs appear:

- confirm your files are `.mp3` or `.wav`
- confirm the music folders in Settings are correct
- save Settings after changing folders so Menulody can rescan

If music does not resume:

- open Menulody once to ensure hooks are installed
- confirm the daemon is running with `menulody --status`
- try disabling and re-enabling `Auto-Start Daemon`

If the now-playing overlay does not appear:

- confirm `Varnish.pak` is installed for your platform
- open `Varnish.pak` and ensure it is enabled
- keep Menulody's `Now Playing Overlay` setting turned on

If playlists look empty:

- some playlist entries may point to files that no longer exist
- re-create the playlist from the current library

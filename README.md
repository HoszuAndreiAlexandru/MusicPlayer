# MusicPlayer

A cross-platform desktop music player prototype built with C++ and Qt 6 Widgets.

The visible application is a native Qt Widgets interface. Spotify sign-in uses the Authorization Code with PKCE flow in the system browser; the app loads the connected user's profile, playlists, and saved tracks through the Spotify Web API. Playback runs through Spotify's Web Playback SDK in a separate headless Chromium-based or Firefox process, while the player controls remain in the native Qt window.

## Requirements

- CMake 3.21 or newer
- Qt 6.5 or newer with the Widgets and Network modules
- A supported C++20 compiler

## Build

This machine has Qt 6.12.0 with the MinGW 13.1 toolchain and Ninja. In PowerShell, configure and build with:

```powershell
$env:Path = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.12.0\mingw_64\bin;$env:Path"
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64 `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe
cmake --build build --parallel
```

On Windows, the build automatically runs `windeployqt` after linking and copies the Qt libraries, platform plugin, and MinGW runtime DLLs beside `build/MusicPlayer.exe`. The MinGW and Qt `bin` directories need to be on `PATH` so the compiler helpers and Qt runtime libraries can be found. On Linux or macOS, use the corresponding Qt installation path and C++ toolchain.

## Connect Spotify

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
2. In MusicPlayer's **Settings**, use the default callback port `43827` or choose another one. Add the displayed `http://127.0.0.1:<port>` Redirect URI to your Spotify app settings exactly, then save the app settings.
3. Copy the app's Client ID into MusicPlayer under **Settings**. Do not enter or embed a Client Secret; this desktop app uses PKCE.
4. Choose **Connect Spotify** on the Home page and approve access in the browser. If you connected before playback access was added, use **Reconnect and grant updated access** in Settings once.
5. In **Settings**, choose Chromium (Chrome / Edge) or Firefox as the headless playback browser. In Search or Your Library, double-click a track or select it and press Play; no playback tab or window should appear. Changing browser selection does not replace an engine that is already running.

The app listens on the configured loopback port for the OAuth callback. It requests `user-read-private`, `user-library-read`, `playlist-read-private`, `playlist-read-collaborative`, `streaming`, and `user-modify-playback-state`. On Windows, the refresh token is DPAPI-protected and saved under the app's local AppData folder, so it survives rebuilds and restarts. The access token stays in memory. Other platforms currently require signing in again after restarting the app.

Library and playlist API responses use a bounded 16 MiB disk cache in Qt's per-user cache folder, outside the build directory. The app follows Spotify's response cache rules and revalidates expired pages; **Refresh** clears cached library pages and fetches them again so added or removed Liked Songs and playlist changes appear immediately. Disconnecting Spotify clears the cache.

## Current scope

- Qt Widgets application shell with Home, Search, Your Library, and Settings navigation
- Dark palette that retains Qt's platform style for standard controls
- Spotify playback through the Web Playback SDK in a selectable headless Chromium or Firefox process, controlled from Qt
- Spotify PKCE sign-in, current-user profile lookup, and Windows AppData session persistence
- Spotify track search with links to open results in Spotify
- Playlist navigation in the sidebar and paginated Liked Songs and playlist views
- Spotify Premium and an installed selected browser with encrypted-media support are required for headless Web Playback SDK audio

The playback companion is served only on loopback with a temporary random access key. The headless browser uses an isolated profile under the user's cache folder, separate from their regular browser profile. The browser must support encrypted media (EME/Widevine) for Spotify playback.

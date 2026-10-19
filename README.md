# Game Output Capture for OBS Studio

This Windows-only OBS source offers two switchable video backends and captures the selected process tree's audio separately. The default is OBS's native Game Capture source, whose graphics hook runs inside the game process and captures graphics API presentation output. The alternate Window/Display backend captures the game window while it is in the background and switches to a delayed, cropped monitor capture while it is foreground.

The source follows native Game Capture availability while keeping active captures opaque:

- Game Capture follows OBS Game Capture hook behavior. The alternate Window/Display backend is cropped to the target window, but monitor capture can include notifications or overlays appearing over the game.
- Captured Window/Display frames are made opaque so an undefined desktop alpha channel cannot make the scene below show through. In Game Capture mode, captured game frames also have transparency disabled. While the selected application is running but the graphics hook has not produced a frame yet, the source covers its target-sized area with opaque black instead of exposing lower scene sources.
- In Game Capture mode, when the selected application is not running, the source has no video dimensions and draws nothing, matching native Game Capture behavior so lower scene sources remain visible. Window/Display mode outputs transparent frames when it has no game image.
- Audio is captured only from the selected process tree, including while the application is in the background.

Test locally before streaming. The video source relies on OBS's native graphics-hook capture; process-loopback audio requires a recent Windows 10/11 release. Hook and audio errors are logged, and the plugin does not fall back to desktop audio or display capture.

## Build

Requirements:

- Windows 10/11 x64
- OBS Studio 32.2.2 x64 development dependencies
- Visual Studio 2022 C++ Build Tools and Windows SDK
- CMake 3.28 or newer

From a Visual Studio Developer PowerShell:

```powershell
cmake --preset windows-ci-x64
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

The CMake preset downloads the OBS 32.2.2 sources and matching OBS dependency archive declared in `buildspec.json`; hashes are verified during download.

## GitHub Releases

Set the version in `buildspec.json` to the intended release version, then push a matching tag to build the Windows package and publish a GitHub release with the ZIP and its SHA-256 checksum. Use a tag such as `0.9.8` for a stable release or `0.9.9-rc1` / `0.9.9-beta1` for a prerelease; do not prefix the tag with `v`.

```powershell
git tag 0.9.8
git push origin 0.9.8
```

## Use in OBS

1. Extract the `game-output-capture` folder from the release archive into `%PROGRAMDATA%\obs-studio\plugins\` (create the `plugins` folder if necessary). The final path should be `%PROGRAMDATA%\obs-studio\plugins\game-output-capture\bin\64bit\game-output-capture.dll`. This project builds x64 and requires 64-bit OBS.
2. Restart OBS and add **Game Output Capture** as a source.
3. Open its properties and choose **Capture method**: **Game Capture** (the default) or **Window and Display**.
4. Set **Mode** to **Capture specific window**, and select the game under **Window**. Use **Refresh windows** to rebuild the list.
5. Set **Window Match Priority** to control fallback matching. **Match title, otherwise find window of same executable** is recommended to keep matching the game after its PID changes.
6. Enable **Capture cursor** if the cursor should appear in the captured video. It is off by default.
7. The source captures audio only from the selected application's process tree and uses Windows-provided audio timestamps. It does not add a manual audio delay or mix in desktop/other-application audio.
8. Test a local recording. Hooking may take a few seconds after selecting the game. In Game Capture mode, while the application is running but not yet hooked, opaque black covers the target area; when the application is not running, the source draws nothing.
9. To switch video backends on the fly, open **Settings → Hotkeys**, find this source's **Toggle capture backend** action, and assign a key. The hotkey toggles the **Capture method** between Game Capture and Window and Display and keeps the property setting in sync.

The selected window is saved using OBS's native `title:class:executable` window-match value, not its PID. The source exposes OBS's **Capture specific window**, **Window**, and **Window Match Priority** settings and passes those same values to the internal `game_capture` source. The selected window remains saved when it is closed; with executable priority, OBS can match the same executable after it restarts. The process-audio monitor applies the same window-match priority and reattaches process-loopback audio when that target appears again. Previous plugin selections are migrated from their saved executable setting when possible.

OBS's top context toolbar is hardcoded for built-in source IDs and cannot be extended by a standalone libobs source plugin. The matching controls are therefore available in this plugin's Properties; adding the same top-row toolbar requires modifying and building the OBS frontend. The video backend is OBS's own `game_capture` source, created privately and rendered through this source; it relies on OBS's injected hook and inherits its anti-cheat and game-compatibility limits. Hook failure while the application is running is covered by an opaque black frame; the source does not fall back to window or monitor capture. This does not increase the OBS output frame rate: a 60 fps OBS output remains limited to 60 output frames per second even if the game presents more. Audio remains Windows process-loopback for the selected process tree; unrelated music-player or browser audio is not included. To avoid mixing those sounds separately into the stream, disable or mute OBS's global **Desktop Audio** source and any other audio source that captures them. Some applications render audio in a separate process that is not a child of the selected process.

The Window/Display backend reuses the plugin's Windows Graphics Capture path: it captures the window in the background, then switches to a delayed monitor capture cropped to the game window after it becomes foreground. The latter can show desktop notifications or overlays that overlap the game. Switching backends changes only video capture; process-loopback audio continues independently.

## Diagnosing audio/video timing

The native Game Capture source reports hook status in the OBS log. The custom source's audio still uses process-loopback timestamps; a recording is needed to assess its alignment with the captured video.

## Limitations

- Windows only; no OBS installer is produced yet.
- Game Capture may fail or be blocked for some protected games or anti-cheat configurations. The manually selected Window/Display backend is an alternative, not an automatic fallback.
- Some applications may render audio in a separate process that is not a child of the selected process.
- Wrapping the native source reuses OBS's game hook, but does not guarantee that every frame generated by third-party frame-generation middleware is visible to the hook.

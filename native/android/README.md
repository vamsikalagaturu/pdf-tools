# PDF Tools for Android (arm64-v8a)

The native window (`native/src/ui`, raylib + raygui + MuPDF) packaged as a debug-signed APK, without Gradle.

```sh
native/android/build.sh          # -> ~/.cache/pdf-tools-native/android/out/pdf-tools.apk
adb install -r ~/.cache/pdf-tools-native/android/out/pdf-tools.apk
```

Requirements: Android SDK at `~/Android/Sdk` (`ANDROID_HOME` overrides) with build-tools 36.1.0, platform
android-36.1 and NDK 28.2.13676358 (`ANDROID_NDK` overrides); JDK (`javac`, `keytool`), CMake, Ninja, GNU make,
zip, curl. Optional: the repo's `.venv` with PyMuPDF, used only to render the launcher icon from `logo.svg`.
All output goes to `~/.cache/pdf-tools-native/android/` (`PT_ANDROID_OUT` overrides); nothing is written into
the repo. Re-runs are incremental.

## What build.sh does

1. Downloads `mupdf-1.28.5-source.tar.gz` (checked against a pinned SHA-256), builds `libmupdf.a` and
   `libmupdf-third.a` with the NDK clang (API 26). A dummy `pkg-config` keeps host libraries out.
2. Configures `native/android/CMakeLists.txt` with the NDK toolchain. It pulls in `native/CMakeLists.txt`
   (for `ptcore`, `ptcli` and `pt_embed`; GUI, PCRE2 and fontconfig off), builds raylib 6.2 from
   `native/thirdparty/raylib` for `PLATFORM_ANDROID` with `raylib_core.c` in place of `rcore.c`, and links every
   `native/src/ui/*.c` (globbed, unmodified) plus the Android files below into `libpdf-tools.so`.
3. Compiles `PdfToolsActivity.java` with `javac` and `d8`, links the manifest and icon with `aapt2`
   (minSdk 26, targetSdk 34, `--debug-mode`), adds `classes.dex` and the stripped `.so`, runs `zipalign -P 16`
   and signs with `apksigner` using a debug keystore created in the cache folder.

targetSdk is 34 because apps targeting 35+ are always drawn edge-to-edge: the system bars and display cutout
would cover the window, and raylib cannot inset its framebuffer.

## Android files

| File | Why |
| --- | --- |
| `android_main.c` | `main()` for raylib's `android_main()`: sets `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR`, `TMPDIR` and `PDF_TOOLS_BUILTIN_CHOOSER=1`, runs app.c's `main`, then exits the process (the shared sources keep state in statics a relaunch would inherit). Turns touch, mouse and key events into widgets.c's GLFW callbacks, characters via `KeyEvent.getUnicodeChar()` (JNI), two-finger drag into wheel scrolling, Back into Escape. Shows the soft keyboard while a text field has focus. Opens `ACTION_VIEW` documents. |
| `glfw_stubs.c` | The GLFW symbols the shared sources call. Cursor, focus and close calls are no-ops; the mouse/key callback setters store widgets.c's handlers; `glfwWaitEventsTimeout` sleeps on the ALooper so an idle window does not redraw at 60 fps. |
| `raylib_core.c` | `#include "rcore.c"` plus `pt_raylib_follow_window()`, which needs raylib's static `CORE`/`platform`: raylib sizes its framebuffer once at window creation and has no public resize on Android. Each frame it sets the buffer geometry to follow the window (0x0, also after a surface rebind), and when the EGL surface size differs from `GetScreenWidth/Height` it updates display/screen/render/current-FBO size, clears the render offset and screen scale, and resets rlgl's framebuffer size, viewport and projection (`SetupViewport`). Scissor and touch scaling read those fields. After a config change, resize or new window, `android_main.c` keeps drawing for 1 s so the surface reaches the new size. If raylib is upgraded, check that the field names still match. |
| `android_glue.h` | Declarations shared by the files above. |
| `java/.../PdfToolsActivity.java` | `NativeActivity` subclass whose `onNewIntent()` calls `setIntent()`, so a PDF opened from another app while the window runs reaches the native side. |
| `AndroidManifest.xml` | Follows the device rotation (no `screenOrientation`); `configChanges` keeps the activity alive through rotation and resizes. `singleTask`, keyboard overlays the window (`adjustNothing`), VIEW filter for `application/pdf`. No permissions. |

### Substitutions in shared sources (keep in sync)

The shared files are compiled unmodified; `CMakeLists.txt` renames a few calls per file with `-D`:

| File | Name | Replacement | Why |
| --- | --- | --- | --- |
| `app.c` | `main` | `pt_app_main` | `android_main.c` provides `main` and calls it after setting up the environment. |
| `app.c` | `InitWindow` | `pt_android_init_window` | Uses `InitWindow(0, 0)` (the whole display, not a 1280x1100 framebuffer scaled into it) and installs the input hook. |
| `app.c` | `file_dialogs_poll` | `pt_android_frame` | Per-frame hook (keyboard, intents, deferred touch press, wheel), then calls the real function. |
| `app.c` | `run_detached` | `pt_android_run_detached` | There is no second window. "Move to new window" reopens the tab in this window instead of losing it. |
| `widgets.c` | `GetCharPressed` | `pt_android_char_pressed` | raylib on Android queues no characters. |
| `widgets.c` | `GetMouseWheelMoveV` | `pt_android_wheel` | raylib on Android reports no wheel; this carries two-finger scroll and mouse wheel. |

If one of these names changes or moves to another file in `native/src/ui`, update `CMakeLists.txt`. New UI
files are picked up automatically; new GLFW calls need a stub in `glfw_stubs.c` (the link uses
`--no-undefined`, so a missing one fails the build).

## Files on the device

- Settings, recent files, signatures: app-internal `files/.config/pdf-tools/` (`HOME` = `files/`).
- PDFs opened from a file manager, mail or browser (`content://` VIEW intents) are copied to
  `cache/opened/<name>` and opened from there. Saving writes to that copy; "Save as" can choose another folder.
- The built-in file browser starts in `files/`. `files/external` links to the app's external folder
  (`/storage/emulated/0/Android/data/io.github.vamsikalagaturu.pdftools/files`), which a PC reaches over USB:
  copy PDFs there to open them, and save there to get results off the device.
- Shared storage (Download, Documents) is not readable without a storage permission, so the browser cannot
  list it; open such files from a file manager instead.

## Touch

Tap = click. Hold (0.15 s) or drag = press and drag. Two fingers = scroll (horizontal and vertical); a press
already sent ends when the second finger lands, which can count as a click. No pinch zoom, no right-click by
touch (a mouse works: right and middle buttons, wheel). Back acts as Escape and never closes the app; use Quit.

## Not available on Android

Printing (no `lp`), SyncTeX (no `synctex` binary), editor commands, new windows and `--forward`, clipboard
(raylib's Android platform has none), regular-expression redaction (built without PCRE2), font matching via
fontconfig. Text input from the soft keyboard is limited to characters that map to key events (no IME
composition, so CJK and similar input do not arrive).

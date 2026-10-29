# PDF Tools for Android (arm64-v8a, Android 11+)

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
   `libmupdf-third.a` with the NDK clang (API 30). A dummy `pkg-config` keeps host libraries out.
2. Downloads `pcre2-10.45.tar.bz2` (pinned SHA-256) and builds a static `libpcre2-8.a` (no JIT) for regex
   search and redaction.
3. Configures `native/android/CMakeLists.txt` with the NDK toolchain. It pulls in `native/CMakeLists.txt`
   (for `ptcore`, `ptcli` and `pt_embed`; GUI and fontconfig off, PCRE2 from step 2), builds raylib 6.2 from
   `native/thirdparty/raylib` for `PLATFORM_ANDROID` with `raylib_core.c` in place of `rcore.c`, and links every
   `native/src/ui/*.c` (globbed, unmodified) plus the Android files below into `libpdf-tools.so`.
4. Compiles `PdfToolsActivity.java` with `javac` and `d8`, links the manifest and icon with `aapt2`
   (minSdk 30, targetSdk 34, `--debug-mode`), adds `classes.dex` and the stripped `.so`, runs `zipalign -P 16`
   and signs with `apksigner` using a debug keystore created in the cache folder.

targetSdk is 34 because apps targeting 35+ are always drawn edge-to-edge: the system bars and display cutout
would cover the window, and raylib cannot inset its framebuffer.

## Android files

| File | Why |
| --- | --- |
| `android_main.c` | `main()` for raylib's `android_main()`: sets `HOME` (shared Download folder with storage access, else `files/`), `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR` and `TMPDIR`, runs app.c's `main`, then exits the process (the shared sources keep state in statics a relaunch would inherit). Turns touch, mouse and key events into widgets.c's GLFW callbacks, characters via `KeyEvent.getUnicodeChar()` (JNI), two-finger drag into wheel scrolling, Back into Escape. Shows the soft keyboard while a text field has focus. Opens `ACTION_VIEW` documents. |
| `glfw_stubs.c` | The GLFW symbols the shared sources call. Cursor, focus and close calls are no-ops; the mouse/key callback setters store widgets.c's handlers; `glfwWaitEventsTimeout` sleeps on the ALooper so an idle window does not redraw at 60 fps. |
| `raylib_core.c` | `#include "rcore.c"` plus `pt_raylib_follow_window()`, which needs raylib's static `CORE`/`platform`: raylib sizes its framebuffer once at window creation and has no public resize on Android. Each frame it sets the buffer geometry to follow the window (0x0, also after a surface rebind), and when the EGL surface size differs from `GetScreenWidth/Height` it updates display/screen/render/current-FBO size, clears the render offset and screen scale, and resets rlgl's framebuffer size, viewport and projection (`SetupViewport`). Scissor and touch scaling read those fields. After a config change, resize or new window, `android_main.c` keeps drawing for 1 s so the surface reaches the new size. If raylib is upgraded, check that the field names still match. |
| `file_dialog_android.c` | `file_dialog`, `file_dialogs_poll`, `file_dialog_busy` and `move_to_trash` for Android: the system document picker, copies of `content://` documents and writing them back (see below). |
| `android_glue.h` | Declarations shared by the files above, including `pt_java()`, which calls a `String` method of the activity. |
| `java/.../PdfToolsActivity.java` | `NativeActivity` subclass: keeps intents delivered while running (`onNewIntent` -> `setIntent`), runs the picker (`startActivityForResult`, result polled by the native side), takes persistable URI permissions, copies documents in and out through `ContentResolver`, maps documents to file paths, and asks for storage access. |
| `AndroidManifest.xml` | Follows the device rotation (no `screenOrientation`); `configChanges` keeps the activity alive through rotation and resizes. `singleTask`, keyboard overlays the window (`adjustNothing`), VIEW filter for `application/pdf`. Storage permissions: READ/WRITE_EXTERNAL_STORAGE up to API 32, MANAGE_EXTERNAL_STORAGE, `requestLegacyExternalStorage` for API 29. |

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
| `file_dialog.c` | `file_dialog`, `file_dialogs_poll`, `file_dialog_busy`, `move_to_trash` | `pt_desktop_*` | `file_dialog_android.c` provides the real ones; its `move_to_trash` still uses `pt_desktop_move_to_trash` for the app's own files. `run_detached` and `split_command` stay as they are. |

If one of these names changes or moves to another file in `native/src/ui`, update `CMakeLists.txt`. New UI
files are picked up automatically; new GLFW calls need a stub in `glfw_stubs.c` (the link uses
`--no-undefined`, so a missing one fails the build).

## Files on the device

Every file dialog is the system document picker:

| Dialog | Picker | What the window gets |
| --- | --- | --- |
| Open (one / several) | `ACTION_OPEN_DOCUMENT` (`EXTRA_ALLOW_MULTIPLE`), types from the patterns: `application/pdf`, `image/*`, else any | the file's own path when storage access maps it to one, else a copy `cache/saf/<hash of URI>/<name>` |
| Save as / save copy | `ACTION_CREATE_DOCUMENT`, suggested name = basename of the start path | the file's own path (the picker created it), else such a copy; the window writes it while the callback runs and it is copied to the document right after |
| Save N files in | `ACTION_OPEN_DOCUMENT_TREE` | the folder's own path, else a new folder `cache/saf-out/...`; each file written there gets a document in the tree (`DocumentsContract.createDocument`) |

Cancel gives an empty list. Picked URIs keep their permission (`takePersistableUriPermission`).

- **Copies and saving back:** each copy's URI is kept in `files/documents.tsv` (path, URI, and the copy's size
  and time when last synced). A watcher (every 0.25 s) writes a copy back to its document
  (`openOutputStream(uri, "wt")`) once it has changed and held still for 0.5 s. So Save (Ctrl+S) on a copy
  replaces the copy as on the desktop and the result reaches the original document. If the document cannot be
  written (for example a read-only grant from another app), the app says so and offers a "save as" picker for the
  saved version; that document then takes the copy's place for later saves.
- **Trash:** Save normally moves the original to the trash first. On Android, files on shared storage and copies
  are replaced in place; only the app's own files (signatures) go to its trash under `files/.local/share/Trash`.
- **Recent files** list copies; they reopen from the copy, and a copy the system removed from the cache is
  fetched again from its document at start-up.
- PDFs opened from a file manager, mail or browser (`ACTION_VIEW`) take the same path (own path or copy).
- Settings and signatures: `files/.config/pdf-tools/`. `files/external` links to the app's external folder,
  which a PC reaches over USB.

### Storage access

With "All files access", picked documents on shared storage
(`/storage/...`, also via the Downloads and media providers) open and save in place by path, and dialogs start
in Download (`HOME`). Without it, everything works through copies and the dialogs start in `files/`.

When the app asks: once at the first start (when its window first gets focus), and later at most once per run,
when a file dialog opens without access (the picker follows when the request returns). The system's
"All files access" page for PDF Tools opens; turn on "Allow access to manage all files", then go back.

## Touch

Tap = click. One finger drags to scroll, with fling momentum after release; a touch stops a fling. A long
press (0.45 s) then drag = press and drag (text selection, moving items and thumbnails); a long press lifted in
place = right click (context menus). Where the UI marks a drag area (`ui_touch_drag_area`: the page under any
tool but View, the signature pad) one finger draws at once. Two fingers scroll and pinch-zoom the page around
the point between them (`ui_add_pinch`). A mouse works too (right and middle buttons, wheel). Back acts as
Escape and never closes the app; use Quit.

## Platform services

- Soft keyboard: `PdfToolsActivity.TextSink`, an invisible view with its own `InputConnection`, receives the
  keyboard's text; composing text is typed into the field as it changes and replaced on commit (backspaces then
  the new text), so CJK, swipe and autocorrect input arrive. `native_text()` queues it for the window thread;
  widgets.c takes backspace (8) and delete (127) among the characters so the order holds.
- Clipboard: `SetClipboardText`/`GetClipboardText` are renamed for every window source to
  `pt_android_set_clipboard`/`pt_android_get_clipboard` (`ClipboardManager`).
- Printing: the edited PDF goes to the system print dialog (`PrintManager`), which picks printer, copies and pages.
- Fonts for text boxes: the NDK font matcher (`AFontMatcher`) with the PDF's family and a generic fallback
  (serif, monospace, sans-serif).
- System bars and keyboard: `systemInsets()` (system bars, cutout and IME, polled every 0.1 s) keeps the UI clear
  of the gesture bar and the soft keyboard; the page view scrolls a focused field above the keyboard.
- Sharing: File > Share... (and the narrow toolbar's More menu) writes the edited PDF to `cache/share/` and opens
  the share sheet; `ShareProvider` serves it read-only as `content://<package>.share/<name>`.

## Not available on Android

Editor commands (inverse SyncTeX search opens nothing), new windows and `--forward`.

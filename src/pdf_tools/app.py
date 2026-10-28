"""Entry point of the pdf-tools command.

Dispatches the window-free commands (cli) and desktop install flags, and otherwise starts the window: one running
app per user, reached over a local socket, so later launches (files, --new-window, --forward) are handed to it.
Also sets up Qt: platform theme for the native file chooser, logging filters and Ctrl+C.
"""

import getpass
import json
import os
import shutil
import signal
import sys
from pathlib import Path

from PySide6.QtCore import QLoggingCategory, QTimer
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import QApplication

from pdf_tools import cli
from pdf_tools.core import desktop
from pdf_tools.core.config import Settings, migrate_legacy
from pdf_tools.ui import icons
from pdf_tools.ui.main_window import MainWindow, dispatch
from pdf_tools.ui.theme import apply_theme, saved_theme
from pdf_tools.ui.util import app_icon

DESKTOP_ID = desktop.DESKTOP_ID
NO_NATIVE_DIALOG_THEMES = ("", "qt5ct", "qt6ct")
USAGE = """usage:
  pdf-tools [file.pdf ...]                       open in the window used last (starts one if needed)
  pdf-tools --new-window [file.pdf ...]          open in a new window
  pdf-tools --forward file.pdf LINE source.tex   SyncTeX: show where a source line is typeset
  pdf-tools --install-desktop | --uninstall-desktop
  pdf-tools COMMAND ...                          without the window, JSON output:
      info redact fill pages merge split protect unprotect flatten synctex
  pdf-tools COMMAND --help                       options of one command
  pdf-tools help                                 the guide for scripts and agents (AGENTS.md)
  pdf-tools --help                               in a terminal: the manual page"""


def request(args):
    """The message for the running app: {"open": [paths], "new_window": bool} or {"forward": [pdf, line, source]}."""
    if args[:1] == ["--forward"]:
        if len(args) != 4 or not args[2].isdigit():
            sys.exit(USAGE)
        return {"forward": [str(Path(args[1]).resolve()), int(args[2]), str(Path(args[3]).resolve())]}
    new_window = args[:1] == ["--new-window"]
    return {"open": [str(Path(a).resolve()) for a in args[new_window:]], "new_window": new_window}


def send_to_running(name, message):
    """Hand `message` to an already running window; False when none is running."""
    sock = QLocalSocket()
    sock.connectToServer(name)
    if not sock.waitForConnected(300):
        return False
    sock.write(json.dumps(message).encode() + b"\n")
    sock.waitForBytesWritten(1000)
    sock.disconnectFromServer()
    return True


def serve(name, handle):
    """Listen for messages from later launches (one JSON object per connection)."""
    server = QLocalServer()
    QLocalServer.removeServer(name)
    server.listen(name)

    def accept():
        sock = server.nextPendingConnection()
        buffer = bytearray()

        def read():
            buffer.extend(bytes(sock.readAll()))
            if buffer.endswith(b"\n"):
                handle(json.loads(buffer.decode()))
                sock.disconnectFromServer()

        sock.readyRead.connect(read)
        sock.disconnected.connect(sock.deleteLater)

    server.newConnection.connect(accept)
    return server


def main():
    """Run what the arguments ask for: a desktop flag, help, a CLI command, or the window.

    For the window, a request is first offered to an app that is already running; only when none answers does this
    process create the window and start listening.
    """
    if sys.argv[1:] in (["--install-desktop"], ["--uninstall-desktop"]):
        install = sys.argv[1] == "--install-desktop"
        paths = desktop.install() if install else desktop.uninstall()
        print("\n".join(f"{'wrote' if install else 'removed'} {p}" for p in paths) or "nothing to remove")
        return
    if sys.argv[1:2] in (["-h"], ["--help"]):
        page = cli.man_page()
        # the manual in a terminal; piped (scripts, agents) or without man, the short usage
        if page and sys.stdout.isatty() and shutil.which("man"):
            os.execvp("man", ["man", "-l", str(page)])
        print(USAGE)
        return
    if sys.argv[1:2] == ["help"]:
        print(cli.guide())
        return
    if sys.argv[1:2] and sys.argv[1] in cli.COMMANDS:
        sys.exit(cli.main(sys.argv[1:]))
    message = request(sys.argv[1:])
    # Qt's event loop never returns to Python, so a Python SIGINT handler would never run; let Ctrl+C kill as usual
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    # harmless Qt Wayland input-method chatter whenever a focused field is removed
    QLoggingCategory.setFilterRules("qt.qpa.wayland.textinput=false")
    # qt5ct/qt6ct have no native file dialog; the portal theme opens the desktop's chooser (with search)
    if os.environ.get("QT_QPA_PLATFORMTHEME", "") in NO_NATIVE_DIALOG_THEMES:
        os.environ["QT_QPA_PLATFORMTHEME"] = "xdgdesktopportal"
    app = QApplication(sys.argv)
    server_name = f"{DESKTOP_ID}-{getpass.getuser()}"
    if send_to_running(server_name, message):
        return
    icons.install_cursors(app)
    app.setApplicationName(DESKTOP_ID)
    app.setDesktopFileName(DESKTOP_ID)
    app.setStyle("Fusion")
    app.setWindowIcon(app_icon())
    migrate_legacy()
    settings = Settings()
    apply_theme(saved_theme(settings))
    win = MainWindow(settings)
    win.show()
    server = serve(server_name, dispatch)
    if message.get("open") or message.get("forward"):
        QTimer.singleShot(0, lambda: win.handle_message(message))
    code = app.exec()
    server.close()
    sys.exit(code)

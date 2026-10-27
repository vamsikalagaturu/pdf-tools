import os
import signal
import sys
from pathlib import Path

from PySide6.QtCore import QLoggingCategory, QTimer
from PySide6.QtWidgets import QApplication

from pdf_tools.core import desktop
from pdf_tools.core.config import Settings, migrate_legacy
from pdf_tools.ui.main_window import MainWindow
from pdf_tools.ui.theme import apply_theme, saved_theme
from pdf_tools.ui.util import app_icon

DESKTOP_ID = desktop.DESKTOP_ID
NO_NATIVE_DIALOG_THEMES = ("", "qt5ct", "qt6ct")


def main():
    if sys.argv[1:] in (["--install-desktop"], ["--uninstall-desktop"]):
        install = sys.argv[1] == "--install-desktop"
        paths = desktop.install() if install else desktop.uninstall()
        print("\n".join(f"{'wrote' if install else 'removed'} {p}" for p in paths) or "nothing to remove")
        return
    # Qt's event loop never returns to Python, so a Python SIGINT handler would never run; let Ctrl+C kill as usual
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    # harmless Qt Wayland input-method chatter whenever a focused field is removed
    QLoggingCategory.setFilterRules("qt.qpa.wayland.textinput=false")
    # qt5ct/qt6ct have no native file dialog; the portal theme opens the desktop's chooser (with search)
    if os.environ.get("QT_QPA_PLATFORMTHEME", "") in NO_NATIVE_DIALOG_THEMES:
        os.environ["QT_QPA_PLATFORMTHEME"] = "xdgdesktopportal"
    app = QApplication(sys.argv)
    app.setApplicationName(DESKTOP_ID)
    app.setDesktopFileName(DESKTOP_ID)
    app.setStyle("Fusion")
    app.setWindowIcon(app_icon())
    migrate_legacy()
    settings = Settings()
    apply_theme(saved_theme(settings))
    win = MainWindow(settings)
    win.show()
    if len(sys.argv) > 1:
        QTimer.singleShot(0, lambda: win.open_paths([Path(a) for a in sys.argv[1:]]))
    sys.exit(app.exec())

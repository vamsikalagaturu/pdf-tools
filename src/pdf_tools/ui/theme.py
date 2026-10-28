from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QApplication

THEMES = ("dark", "light")
# palette() references resolve when the sheet is set, so re-apply after a theme change
HEADER_STYLE = """
QToolBar { border: none; spacing: 2px; padding: 0px; }
QToolBar::separator { width: 1px; background: palette(mid); margin: 5px 6px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 3px 6px; }
QToolButton:hover { background: palette(button); border-color: palette(mid); }
QToolButton:pressed, QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }
QToolButton[popupMode="1"] { padding-right: 18px; }
QToolButton::menu-button { border: none; width: 14px; }
QToolButton#menu::menu-indicator { image: none; width: 0px; }
"""
DOC_TAB_STYLE = """
QTabBar::tab { background: transparent; border: none; border-bottom: 2px solid transparent;
               padding: 5px 8px 4px 12px; margin-right: 1px; color: palette(window-text); max-width: 260px; }
QTabBar::tab:hover:!selected { background: palette(button); }
QTabBar::tab:selected { background: palette(button); border-bottom: 2px solid palette(highlight); }
QTabBar QToolButton { border: none; border-radius: 4px; padding: 2px; margin-left: 6px; margin-right: 7px; }
QTabBar QToolButton:hover { background: palette(mid); }
"""


def dark_palette():
    p = QPalette()
    for role, colour in {
        QPalette.Window: "#2b2b2b",
        QPalette.WindowText: "#e6e6e6",
        QPalette.Base: "#1f1f1f",
        QPalette.AlternateBase: "#2b2b2b",
        QPalette.ToolTipBase: "#2b2b2b",
        QPalette.ToolTipText: "#e6e6e6",
        QPalette.PlaceholderText: "#8a8a8a",
        QPalette.Text: "#e6e6e6",
        QPalette.Button: "#353535",
        QPalette.ButtonText: "#e6e6e6",
        QPalette.BrightText: "#ff6b6b",
        QPalette.Light: "#4a4a4a",
        QPalette.Midlight: "#3e3e3e",
        QPalette.Mid: "#2f2f2f",
        QPalette.Dark: "#1a1a1a",
        QPalette.Shadow: "#000000",
        QPalette.Link: "#6ab0ff",
        QPalette.Highlight: "#2f6fd0",
        QPalette.HighlightedText: "#ffffff",
    }.items():
        p.setColor(role, QColor(colour))
    for role in (QPalette.WindowText, QPalette.Text, QPalette.ButtonText):
        p.setColor(QPalette.Disabled, role, QColor("#7a7a7a"))
    return p


def saved_theme(settings):
    mode = settings.get("theme")
    return mode if mode in THEMES else THEMES[0]


def apply_theme(mode):
    app = QApplication.instance()
    palette = dark_palette() if mode == "dark" else app.style().standardPalette()
    app.setPalette(palette)
    # an app palette change stops propagating below containers holding style-sheeted children; set it everywhere
    for widget in app.allWidgets():
        widget.setPalette(palette)

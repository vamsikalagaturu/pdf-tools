"""Application windows and how launches reach them.

A MainWindow is a header (menu button, Home logo, the current tab's toolbar) above one tab strip holding
document tabs (EditTab) and, on demand, the Home and Combine & split tabs. All windows live in one process:
MainWindow.windows is the registry, and dispatch() routes requests from later `pdf-tools` launches.
"""

from pathlib import Path

import pymupdf
from PySide6.QtCore import QEvent, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QMainWindow,
    QMenu,
    QMessageBox,
    QStackedWidget,
    QTabBar,
    QTabWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from pdf_tools import APP_NAME, REPO_URL, __version__
from pdf_tools.ui import icons
from pdf_tools.ui.edit_tab import EditTab
from pdf_tools.ui.workspace_tab import IMAGE_SUFFIXES, WorkspacePage
from pdf_tools.ui.start_page import StartPage
from pdf_tools.ui.theme import DOC_TAB_STYLE, HEADER_STYLE, THEMES, apply_theme, saved_theme
from pdf_tools.ui.util import app_icon


def dispatch(message):
    """Route a launch request: a forward search to the window showing that PDF, files to the window used last (or
    to a new one with "new_window"); a file already open elsewhere is shown where it is."""
    windows = MainWindow.windows
    target = None
    if message.get("forward"):
        target = next((w for w in windows if w.tab_for(message["forward"][0])), None)
    if message.get("open") and not message.get("new_window"):
        for path in list(message["open"]):
            owner = next((w for w in windows if w.tab_for(path)), None)
            if owner:
                owner.docs.setCurrentWidget(owner.tab_for(path))
                owner.raise_()
                owner.activateWindow()
                message = dict(message, open=[p for p in message["open"] if p != path])
        if not message["open"] and not message.get("forward"):
            return
    if target is None:
        last = MainWindow.last_active or windows[-1]
        target = last.new_window() if message.get("new_window") else last
    target.handle_message(message)


class MainWindow(QMainWindow):
    """Header (menu, home logo, current tab's toolbar) above one tab strip: documents, plus Home and Combine & split
    on demand.

    `windows` lists the open windows in creation order; `last_active` is the one used last, where new files go.
    """

    windows = []
    last_active = None

    def __init__(self, settings):
        """Register the window, build its header, tabs and actions, and start on a Home tab. Closing deletes it."""
        super().__init__()
        self.settings = settings
        self.setAttribute(Qt.WA_DeleteOnClose)
        MainWindow.windows.append(self)
        MainWindow.last_active = self
        self.setAcceptDrops(True)
        self.setWindowIcon(app_icon())
        self.resize(1280, 1100)
        self.menu_actions = []
        self.info_widget = None

        self.start = StartPage(settings)
        self.workspace = WorkspacePage(Path.home())
        self.docs = QTabWidget()
        self.docs.setDocumentMode(True)
        self.docs.setTabsClosable(True)
        self.docs.setMovable(True)
        self.docs.setElideMode(Qt.ElideMiddle)
        self.docs.tabBar().setExpanding(False)
        self.docs.setStyleSheet(DOC_TAB_STYLE)
        self.docs.tabCloseRequested.connect(lambda i: self.close_tab(self.docs.widget(i)))
        self.docs.tabBar().setContextMenuPolicy(Qt.CustomContextMenu)
        self.docs.tabBar().customContextMenuRequested.connect(self.on_tab_menu)
        self.blank_toolbar = QWidget()
        self.toolbars = QStackedWidget()
        self.toolbars.addWidget(self.blank_toolbar)
        self.toolbars.addWidget(self.workspace.toolbar)

        self.start.open_requested.connect(lambda path: self.open_paths([path]) if path else self.open_dialog())
        self.start.workspace_requested.connect(lambda: self.open_workspace())
        self.workspace.open_requested.connect(lambda path: self.open_paths([path]))

        self.menu_button = QToolButton()
        self.menu_button.setText("Menu")
        icons.bind(self.menu_button, "menu")
        self.menu_button.setToolTip("Menu")
        self.menu_button.setPopupMode(QToolButton.InstantPopup)
        self.menu_button.setObjectName("menu")
        self.logo = QToolButton()
        self.logo.setIcon(app_icon())
        self.logo.setIconSize(QSize(22, 22))
        self.logo.setToolTip("Home")
        self.logo.setCursor(Qt.PointingHandCursor)
        self.logo.clicked.connect(self.go_home)
        self.build_window_actions()

        self.header = QWidget()
        row = QHBoxLayout(self.header)
        row.setContentsMargins(6, 3, 6, 3)
        row.setSpacing(8)
        row.addWidget(self.menu_button)
        row.addWidget(self.logo)
        row.addWidget(self.toolbars, 1)
        self.header.setStyleSheet(HEADER_STYLE)
        central = QWidget()
        col = QVBoxLayout(central)
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(0)
        col.addWidget(self.header)
        col.addWidget(self.docs, 1)
        self.setCentralWidget(central)
        self.docs.currentChanged.connect(lambda _: self.refresh())
        self.go_home()

    def action(self, text, slot, shortcut=None, icon=None):
        a = QAction(text, self)
        a.triggered.connect(lambda checked=False: slot())
        if shortcut:
            a.setShortcut(QKeySequence(shortcut))
        if icon:
            icons.bind(a, icon)
        return a

    def build_window_actions(self):
        """Actions that exist without a document: open, close tab, merge, quit, tab switching, theme."""
        self.a_open = self.action("Open...", self.open_dialog, QKeySequence.Open, "open")
        self.a_close_tab = self.action("Close tab", lambda: self.close_tab(self.docs.currentWidget()), "Ctrl+W", "close")
        self.a_merge = self.action("Combine && split PDFs...", self.open_workspace, "Ctrl+M", "merge")
        self.a_quit = self.action("Quit", QApplication.closeAllWindows, QKeySequence.Quit)
        self.a_new_window = self.action("New window", self.new_window, QKeySequence.New)
        self.a_close_window = self.action("Close window", self.close, "Ctrl+Shift+W")
        self.a_about = self.action(f"About {APP_NAME}", self.about)
        self.a_home = self.action("Home", self.go_home)
        self.a_home.setIcon(app_icon())
        self.recent_menu = QMenu("Open recent", self)
        self.recent_menu.aboutToShow.connect(self.fill_recent_menu)
        self.a_next_tab = self.action("Next tab", lambda: self.step_tab(1), "Ctrl+Tab")
        self.a_prev_tab = self.action("Previous tab", lambda: self.step_tab(-1), "Ctrl+Shift+Tab")
        self.appearance = QMenu("Appearance", self)
        group = QActionGroup(self)
        for mode in THEMES:
            a = QAction(mode.capitalize(), self)
            a.setCheckable(True)
            a.setData(mode)
            a.triggered.connect(lambda checked=False, mode=mode: self.set_theme(mode))
            group.addAction(a)
            self.appearance.addAction(a)
        self.sync_theme_actions()

    def sync_theme_actions(self):
        current = saved_theme(self.settings)
        for a in self.appearance.actions():
            a.setChecked(a.data() == current)

    def new_window(self):
        window = MainWindow(self.settings)
        window.resize(self.size())
        window.show()
        return window

    def tab_for(self, path):
        """The document tab showing `path` in this window, or None."""
        path = Path(path).resolve()
        return next((t for t in self.tabs() if t.doc and t.doc_path.resolve() == path), None)

    def on_tab_menu(self, pos):
        widget = self.docs.widget(self.docs.tabBar().tabAt(pos))
        if not isinstance(widget, EditTab):
            return
        menu = QMenu(self)
        menu.addAction("Move to new window", lambda: self.move_to_new_window(widget))
        menu.addAction("Close", lambda: self.close_tab(widget))
        menu.exec(self.docs.tabBar().mapToGlobal(pos))

    def move_to_new_window(self, tab):
        """Hand a document tab (with its edits and undo history) to a fresh window."""
        window = self.new_window()
        self.release(tab)
        window.adopt(tab)
        window.docs.setCurrentWidget(tab)
        window.drop_page(window.start)

    def changeEvent(self, e):
        """Remember this window as the one used last whenever it is activated (dispatch() sends files there)."""
        if e.type() == QEvent.ActivationChange and self.isActiveWindow():
            MainWindow.last_active = self
        super().changeEvent(e)

    def fill_recent_menu(self):
        self.recent_menu.clear()
        for path in self.settings.get("recent_files"):
            if Path(path).exists():
                a = self.recent_menu.addAction(Path(path).name, lambda path=path: self.open_paths([path]))
                a.setToolTip(path)
        if self.recent_menu.isEmpty():
            self.recent_menu.addAction("No recent files").setEnabled(False)

    @property
    def edit(self):
        current = self.docs.currentWidget()
        return current if isinstance(current, EditTab) else None

    def tabs(self):
        """Open documents (every tab except Home and Combine & split)."""
        return [w for w in map(self.docs.widget, range(self.docs.count())) if isinstance(w, EditTab)]

    def add_tab(self, widget, title, icon=None):
        """Insert `widget` after the current tab with a close button, or switch to it if it is already open."""
        index = self.docs.indexOf(widget)
        if index < 0:
            index = self.docs.insertTab(self.docs.currentIndex() + 1, widget, title)
            if icon:
                self.docs.setTabIcon(index, icon)
            close = QToolButton()
            close.setAutoRaise(True)
            close.setToolTip("Close (Ctrl+W)")
            close.setIconSize(QSize(14, 14))
            close.setCursor(Qt.PointingHandCursor)
            icons.bind(close, "close")
            close.clicked.connect(lambda _=False, w=widget: self.close_tab(w))
            self.docs.tabBar().setTabButton(index, QTabBar.RightSide, close)
        return index

    def go_home(self):
        """Switch to the Home tab, opening one next to the current tab if none is open (like a browser new tab)."""
        self.add_tab(self.start, "Home", app_icon())
        self.docs.setCurrentWidget(self.start)

    def open_workspace(self, tab=None):
        """Open (or switch to) the Combine & split tab, adding document tab `tab` (its edits applied) when given;
        started from Home, it takes Home's place."""
        from_home = self.docs.currentWidget() is self.start
        if tab is not None and tab.doc:
            self.workspace.add_document(pymupdf.open("pdf", tab.edited()[0]), tab.doc_path, tab.password)
        self.add_tab(self.workspace, "Combine && split", icons.icon("merge"))
        self.docs.setCurrentWidget(self.workspace)
        if from_home:
            self.drop_page(self.start)

    def drop_page(self, page):
        """Remove the Home or Combine & split tab (the widget is kept for reuse); the window always keeps a tab."""
        i = self.docs.indexOf(page)
        if i >= 0 and self.docs.count() > 1:
            self.docs.removeTab(i)

    def open_dialog(self):
        start = self.settings.get("last_folder") or str(Path.home())
        paths, _ = QFileDialog.getOpenFileNames(self, "Open PDFs", start, "PDF (*.pdf)")
        self.open_paths(paths)

    def open_paths(self, paths):
        """Open each PDF in its own tab, or switch to the tab that already shows it; opened from Home, they replace it."""
        from_home = self.docs.currentWidget() is self.start
        for path in paths:
            path = Path(path).resolve()
            tab = next((t for t in self.tabs() if t.doc and t.doc_path.resolve() == path), None)
            if tab is None:
                tab = self.new_tab()
                self.docs.setCurrentWidget(tab)
                try:
                    loaded = tab.load(path)
                except Exception as e:
                    QMessageBox.critical(self, APP_NAME, f"Cannot open {path.name}:\n{e}")
                    loaded = None
                if not loaded:
                    self.remove_tab(tab)
                    continue
                # the tab was just made visible; fit once its size is laid out
                QTimer.singleShot(0, lambda t=tab: (t.fit_width(), t.scroll_home()))
            self.docs.setCurrentWidget(tab)
        if from_home and self.tabs():
            self.drop_page(self.start)

    def handle_message(self, message):
        """A launch request: {"open": [paths]} or {"forward": [pdf, line, source]} (SyncTeX forward search)."""
        if message.get("forward"):
            pdf, line, source = message["forward"]
            self.open_paths([pdf])
            tab = next((t for t in self.tabs() if t.doc and t.doc_path.resolve() == Path(pdf).resolve()), None)
            if tab:
                # after open_paths' deferred fit, which would scroll back to the top
                QTimer.singleShot(0, lambda: tab.forward_search(source, line))
        elif message.get("open"):
            self.open_paths(message["open"])
        self.raise_()
        self.activateWindow()

    def new_tab(self):
        tab = EditTab(self.settings)
        self.adopt(tab)
        return tab

    def adopt(self, tab):
        """Show document tab `tab` (new, or released by another window) in this window."""
        tab.active = False
        tab.activate_requested.connect(lambda t=tab: self.docs.setCurrentWidget(t))
        tab.closed.connect(lambda t=tab: self.remove_tab(t))
        tab.open_dialog_requested.connect(self.open_dialog)
        tab.title_changed.connect(lambda title, t=tab: self.set_tab_title(t, title))
        tab.split_requested.connect(self.open_workspace)
        self.toolbars.addWidget(tab.toolbar)
        self.add_tab(tab, "")
        if tab.doc:
            tab.update_state()

    def release(self, tab):
        """Take document tab `tab` out of this window without closing it (see adopt)."""
        for signal in (tab.activate_requested, tab.closed, tab.open_dialog_requested, tab.title_changed, tab.split_requested):
            signal.disconnect()
        if self.info_widget is tab:
            self.swap_info(None)
        self.docs.removeTab(self.docs.indexOf(tab))
        self.toolbars.removeWidget(tab.toolbar)
        if not self.docs.count():
            self.go_home()

    def set_tab_title(self, tab, title):
        i = self.docs.indexOf(tab)
        if i >= 0:
            self.docs.setTabText(i, title)
            self.docs.setTabToolTip(i, str(tab.doc_path))

    def close_tab(self, widget):
        """Close any tab: Home and Combine & split are only taken out (kept for reuse), a document asks about
        unsaved edits first. The window is never left without a tab; Home fills in."""
        if widget is self.workspace and self.docs.count() == 1:
            # the last tab: Home takes its place, as when the last document closes
            self.go_home()
        if widget in (self.start, self.workspace):
            self.drop_page(widget)
        elif widget is not None:
            if widget.doc:
                widget.close_doc()
            else:
                self.remove_tab(widget)
        if not self.docs.count():
            self.go_home()

    def remove_tab(self, tab):
        """Delete a closed document tab with its toolbar and status widget (see release() to move one instead)."""
        if self.info_widget is tab:
            self.swap_info(None)
        self.docs.removeTab(self.docs.indexOf(tab))
        self.toolbars.removeWidget(tab.toolbar)
        tab.toolbar.deleteLater()
        tab.deleteLater()
        if not self.docs.count():
            self.go_home()

    def step_tab(self, delta):
        if self.docs.count() > 1:
            self.docs.setCurrentIndex((self.docs.currentIndex() + delta) % self.docs.count())

    def refresh(self):
        """Show the current tab's toolbar, status line, menu and window title."""
        current = self.docs.currentWidget()
        tab = self.edit
        workspace = current is self.workspace
        self.toolbars.setCurrentWidget(tab.toolbar if tab else self.workspace.toolbar if workspace else self.blank_toolbar)
        for t in self.tabs():
            t.active = t is tab
        self.swap_info(tab)
        self.rebuild_menu(tab)
        if tab:
            tab.update_state()
        elif workspace:
            self.setWindowTitle(f"Combine & split - {APP_NAME}")
        else:
            self.start.refresh()
            self.setWindowTitle(APP_NAME)

    def swap_info(self, tab):
        """Show document tab `tab`'s status widgets, or none for other tabs: its page and edit counts on the left
        (where temporary messages briefly cover them) and its file path on the right."""
        bar = self.statusBar()
        if self.info_widget is not None:
            bar.removeWidget(self.info_widget.stats)
            bar.removeWidget(self.info_widget.info)
            self.info_widget = None
        if tab:
            bar.addWidget(tab.stats, 1)
            bar.addPermanentWidget(tab.info)
            if bar.currentMessage():
                # hidden the way QStatusBar hides left widgets during a message, so it shows them again after it
                tab.stats.hide()
                tab.stats.setAttribute(Qt.WA_WState_ExplicitShowHide, False)
            else:
                tab.stats.show()
            tab.info.show()
            self.info_widget = tab

    def rebuild_menu(self, e):
        """The menu (and its window-level shortcuts) points at the active document's actions."""
        for a in self.menu_actions:
            self.removeAction(a)
        menu = QMenu(self)
        f = menu.addMenu("File")
        f.addActions([self.a_new_window, self.a_home, self.a_open])
        f.addMenu(self.recent_menu)
        f.addActions([self.a_close_tab, self.a_close_window])
        if e:
            f.addSeparator()
            f.addActions([e.a_save, e.a_save_as, e.a_export])
            f.addSeparator()
            f.addActions([e.a_protect, e.a_unprotect])
            f.addSeparator()
            f.addActions([e.a_print, e.a_print_preview])
            f.addSeparator()
            f.addAction(e.a_copy_path)
        f.addSeparator()
        f.addAction(self.a_merge)
        if e:
            f.addAction(e.a_split)
        f.addSeparator()
        f.addAction(self.a_quit)
        subs = [f]
        if e:
            m = menu.addMenu("Edit")
            m.addActions([e.a_undo, e.a_redo])
            m.addSeparator()
            m.addActions([e.a_copy, e.a_delete])
            m.addSeparator()
            m.addActions([e.a_clear_page, e.a_clear_all])
            m.addSeparator()
            m.addAction(e.a_find)
            t = menu.addMenu("Tools")
            t.addActions(list(e.tool_actions.values()))
            t.addSeparator()
            t.addActions([e.a_snap, e.a_auto, e.a_redact_colour])
            t.addSeparator()
            t.addMenu(e.style_menu)
            t.addMenu(e.colour_menu)
            p = menu.addMenu("Pages")
            p.addActions([e.a_rotate_left, e.a_rotate_right])
            p.addSeparator()
            p.addActions([e.a_fit_content, e.a_uncrop])
            p.addSeparator()
            p.addActions([e.a_insert_pages, e.a_remove_pages])
            p.addSeparator()
            p.addActions([e.a_split, e.a_tick, e.a_untick])
            subs += [m, t, p]
        v = menu.addMenu("View")
        if e:
            v.addActions([e.a_sidebar, e.a_continuous])
            v.addSeparator()
            v.addActions([e.a_zoom_in, e.a_zoom_out, e.a_fit])
            v.addSeparator()
            v.addActions([e.a_prev, e.a_next])
            v.addSeparator()
        v.addActions([self.a_next_tab, self.a_prev_tab])
        v.addSeparator()
        v.addMenu(self.appearance)
        subs.append(v)
        menu.addSeparator()
        menu.addAction(self.a_about)
        old = self.menu_button.menu()
        self.menu_button.setMenu(menu)
        if old:
            old.deleteLater()
        # shortcuts only fire for actions attached to a visible widget, and the menu is hidden until opened
        self.menu_actions = [a for sub in subs for a in sub.actions() if not a.isSeparator() and not a.menu()]
        self.addActions(self.menu_actions)

    def set_theme(self, mode):
        """Switch the app-wide theme, remember it, and restyle every open window."""
        self.settings.set("theme", mode)
        apply_theme(mode)
        icons.refresh()
        for window in MainWindow.windows:
            window.restyle()

    def restyle(self):
        self.sync_theme_actions()
        self.header.setStyleSheet(HEADER_STYLE)
        self.docs.setStyleSheet(DOC_TAB_STYLE)
        self.workspace.restyle()
        if self.docs.indexOf(self.workspace) >= 0:
            self.docs.setTabIcon(self.docs.indexOf(self.workspace), icons.icon("merge"))
        for t in self.tabs():
            t.edits_sig = None
            t.update_state()

    def about(self):
        QMessageBox.about(
            self,
            f"About {APP_NAME}",
            f"<h3>{APP_NAME}</h3>"
            f"<p>Version {__version__}</p>"
            "<p>View, redact, fill, annotate, sign and merge PDFs.</p>"
            f'<p><a href="{REPO_URL}">{REPO_URL.removeprefix("https://")}</a></p>'
            "<p>License: AGPL-3.0-or-later. Icons: Lucide (ISC).</p>",
        )

    def closeEvent(self, e):
        """Ask once about unsaved edits in this window's documents, close them and leave the window registry
        (the app quits when the last window closes)."""
        dirty =[t for t in self.tabs() if t.edit_count()]
        if dirty:
            names = "\n".join(f"  {t.doc_path.name}" for t in dirty)
            reply = QMessageBox.question(
                self, APP_NAME, f"{len(dirty)} document(s) have unsaved edits:\n{names}\n\nClose and discard them?"
            )
            if reply != QMessageBox.Yes:
                e.ignore()
                return
        for t in self.tabs():
            # close documents while their scenes still exist (see EditTab.unload)
            t.unload()
        if self in MainWindow.windows:
            MainWindow.windows.remove(self)
        if MainWindow.last_active is self:
            MainWindow.last_active = MainWindow.windows[-1] if MainWindow.windows else None
        e.accept()

    def droppable(self):
        """File types a drop accepts here: images too while Combine & split is showing."""
        return (".pdf", *IMAGE_SUFFIXES) if self.docs.currentWidget() is self.workspace else (".pdf",)

    def dragEnterEvent(self, e):
        if any(u.toLocalFile().lower().endswith(self.droppable()) for u in e.mimeData().urls()):
            e.acceptProposedAction()

    def dropEvent(self, e):
        """Files dropped on the window open as tabs, or join Combine & split when that tab is showing."""
        files =[u.toLocalFile() for u in e.mimeData().urls() if u.toLocalFile().lower().endswith(self.droppable())]
        if self.docs.currentWidget() is self.workspace:
            self.workspace.add_paths(files)
        elif files:
            self.open_paths(files)

from pathlib import Path

from PySide6.QtCore import QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QKeySequence
from PySide6.QtWidgets import (
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
from pdf_tools.ui.merge_tab import MergePage
from pdf_tools.ui.start_page import StartPage
from pdf_tools.ui.theme import DOC_TAB_STYLE, HEADER_STYLE, THEMES, apply_theme, saved_theme
from pdf_tools.ui.util import app_icon


class MainWindow(QMainWindow):
    """Header (menu, home logo, current tab's toolbar) above one tab strip: documents, plus Home and Merge on demand."""

    def __init__(self, settings):
        super().__init__()
        self.settings = settings
        self.setAcceptDrops(True)
        self.setWindowIcon(app_icon())
        self.resize(1280, 1100)
        self.menu_actions = []
        self.info_widget = None

        self.start = StartPage(settings)
        self.merge_page = MergePage(Path.home())
        self.docs = QTabWidget()
        self.docs.setDocumentMode(True)
        self.docs.setTabsClosable(True)
        self.docs.setMovable(True)
        self.docs.setElideMode(Qt.ElideMiddle)
        self.docs.tabBar().setExpanding(False)
        self.docs.setStyleSheet(DOC_TAB_STYLE)
        self.docs.tabCloseRequested.connect(lambda i: self.close_tab(self.docs.widget(i)))
        self.blank_toolbar = QWidget()
        self.toolbars = QStackedWidget()
        self.toolbars.addWidget(self.blank_toolbar)
        self.toolbars.addWidget(self.merge_page.toolbar)

        self.start.open_requested.connect(lambda path: self.open_paths([path]) if path else self.open_dialog())
        self.start.merge_requested.connect(self.open_merge)
        self.merge_page.open_requested.connect(lambda path: self.open_paths([path]))

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
        self.a_merge = self.action("Merge PDFs...", self.open_merge, "Ctrl+M", "merge")
        self.a_quit = self.action("Quit", self.close, QKeySequence.Quit)
        self.a_about = self.action(f"About {APP_NAME}", self.about, icon="about")
        self.a_next_tab = self.action("Next tab", lambda: self.step_tab(1), "Ctrl+Tab")
        self.a_prev_tab = self.action("Previous tab", lambda: self.step_tab(-1), "Ctrl+Shift+Tab")
        self.appearance = QMenu("Appearance", self)
        group = QActionGroup(self)
        current = saved_theme(self.settings)
        for mode in THEMES:
            a = QAction(mode.capitalize(), self)
            a.setCheckable(True)
            a.setChecked(mode == current)
            a.triggered.connect(lambda checked=False, mode=mode: self.set_theme(mode))
            group.addAction(a)
            self.appearance.addAction(a)

    @property
    def edit(self):
        current = self.docs.currentWidget()
        return current if isinstance(current, EditTab) else None

    def tabs(self):
        """Open documents (every tab except Home and Merge)."""
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

    def open_merge(self):
        """Open (or switch to) the Merge tab; started from Home, it takes Home's place."""
        from_home = self.docs.currentWidget() is self.start
        self.add_tab(self.merge_page, "Merge PDFs", icons.icon("merge"))
        self.docs.setCurrentWidget(self.merge_page)
        if from_home:
            self.drop_page(self.start)

    def drop_page(self, page):
        """Remove the Home or Merge tab (the widget is kept for reuse); the window is never left without a tab."""
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
        tab.active = False
        tab.activate_requested.connect(lambda t=tab: self.docs.setCurrentWidget(t))
        tab.closed.connect(lambda t=tab: self.remove_tab(t))
        tab.open_dialog_requested.connect(self.open_dialog)
        tab.title_changed.connect(lambda title, t=tab: self.set_tab_title(t, title))
        self.toolbars.addWidget(tab.toolbar)
        self.add_tab(tab, "")
        return tab

    def set_tab_title(self, tab, title):
        i = self.docs.indexOf(tab)
        if i >= 0:
            self.docs.setTabText(i, title)
            self.docs.setTabToolTip(i, str(tab.doc_path))

    def close_tab(self, widget):
        if widget is self.start or widget is self.merge_page:
            self.drop_page(widget)
        elif widget is not None:
            if widget.doc:
                widget.close_doc()
            else:
                self.remove_tab(widget)
        if not self.docs.count():
            self.go_home()

    def remove_tab(self, tab):
        if self.info_widget is tab.info:
            self.statusBar().removeWidget(tab.info)
            self.info_widget = None
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
        self.toolbars.setCurrentWidget(
            tab.toolbar if tab else self.merge_page.toolbar if current is self.merge_page else self.blank_toolbar
        )
        for t in self.tabs():
            t.active = t is tab
        self.swap_info(tab)
        self.rebuild_menu(tab)
        if tab:
            tab.update_state()
        elif current is self.merge_page:
            self.setWindowTitle(f"Merge PDFs - {APP_NAME}")
        else:
            self.start.refresh()
            self.setWindowTitle(APP_NAME)

    def swap_info(self, tab):
        if self.info_widget is not None:
            self.statusBar().removeWidget(self.info_widget)
            self.info_widget = None
        if tab:
            self.statusBar().addPermanentWidget(tab.info)
            tab.info.show()
            self.info_widget = tab.info

    def rebuild_menu(self, e):
        """The menu (and its window-level shortcuts) points at the active document's actions."""
        for a in self.menu_actions:
            self.removeAction(a)
        menu = QMenu(self)
        f = menu.addMenu("File")
        f.addActions([self.a_open, self.a_close_tab])
        if e:
            f.addSeparator()
            f.addActions([e.a_save, e.a_save_as, e.a_export, e.a_split])
            f.addSeparator()
            f.addActions([e.a_protect, e.a_unprotect])
            f.addSeparator()
            f.addActions([e.a_print, e.a_print_preview])
        f.addSeparator()
        f.addAction(self.a_merge)
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
            m.addSeparator()
            m.addActions([e.a_tick, e.a_untick])
            t = menu.addMenu("Tools")
            t.addActions(list(e.tool_actions.values()))
            t.addSeparator()
            t.addActions([e.a_snap, e.a_auto])
            t.addSeparator()
            t.addAction(e.a_highlight_colour)
            p = menu.addMenu("Pages")
            p.addActions([e.a_rotate_left, e.a_rotate_right])
            p.addSeparator()
            p.addActions([e.a_fit_content, e.a_uncrop])
            p.addSeparator()
            p.addActions([e.a_insert_pages, e.a_remove_pages])
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
        self.settings.set("theme", mode)
        apply_theme(mode)
        icons.refresh()
        self.header.setStyleSheet(HEADER_STYLE)
        self.docs.setStyleSheet(DOC_TAB_STYLE)
        self.merge_page.restyle()
        merge_index = self.docs.indexOf(self.merge_page)
        if merge_index >= 0:
            self.docs.setTabIcon(merge_index, icons.icon("merge"))
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
        dirty = [t for t in self.tabs() if t.edit_count()]
        if dirty:
            names = "\n".join(f"  {t.doc_path.name}" for t in dirty)
            reply = QMessageBox.question(
                self, APP_NAME, f"{len(dirty)} document(s) have unsaved edits:\n{names}\n\nQuit and discard them?"
            )
            if reply != QMessageBox.Yes:
                e.ignore()
                return
        e.accept()

    def dragEnterEvent(self, e):
        if any(u.toLocalFile().lower().endswith(".pdf") for u in e.mimeData().urls()):
            e.acceptProposedAction()

    def dropEvent(self, e):
        pdfs = [u.toLocalFile() for u in e.mimeData().urls() if u.toLocalFile().lower().endswith(".pdf")]
        if self.docs.currentWidget() is self.merge_page:
            self.merge_page.add_paths(pdfs)
        elif pdfs:
            self.open_paths(pdfs)

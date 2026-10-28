from PySide6.QtCore import QEvent, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QAbstractItemView, QHeaderView, QToolButton, QTreeWidget, QTreeWidgetItem

from pdf_tools.ui import icons

LIST_STYLE = """
QTreeWidget::item { padding: 6px 4px; border-bottom: 1px solid rgba(128, 128, 128, 70); }
QTreeWidget::item:has-children { padding: 8px 4px 4px 4px; border-bottom: none; }
"""
DISCARD_STYLE = """
QToolButton { border: none; border-radius: 4px; padding: 2px; }
QToolButton:hover { background: rgb(220, 60, 60); }
"""


class EditsPanel(QTreeWidget):
    """Unsaved edits grouped under headings; the hovered row shows an X that discards it (or its whole group)."""

    discard_requested = Signal(list)
    activated = Signal(object)

    def __init__(self):
        super().__init__()
        self.setHeaderHidden(True)
        self.setColumnCount(2)
        self.setIndentation(12)
        self.setIconSize(QSize(14, 14))
        self.setStyleSheet(LIST_STYLE)
        header = self.header()
        header.setStretchLastSection(False)
        header.setSectionResizeMode(0, QHeaderView.Stretch)
        header.setSectionResizeMode(1, QHeaderView.Fixed)
        header.resizeSection(1, 30)
        self.setMouseTracking(True)
        self.itemEntered.connect(lambda item, _: self.show_discard(item))
        self.itemClicked.connect(lambda item, _: self.activated.emit(item.data(0, Qt.UserRole)))
        self.viewport().installEventFilter(self)

    def set_groups(self, groups):
        """groups: [(title, ref, [(label, detail, icon name, ref)])]; refs come back in the signals."""
        self.clear()
        bold = self.font()
        bold.setBold(True)
        for title, group_ref, rows in groups:
            top = QTreeWidgetItem([f"{title}  ({len(rows)})"])
            top.setFlags(Qt.ItemIsEnabled)
            top.setFont(0, bold)
            top.setData(0, Qt.UserRole, group_ref)
            self.addTopLevelItem(top)
            self.add_discard(top, [ref for *_, ref in rows], f"Discard all edits under {title}")
            for label, detail, icon, ref in rows:
                child = QTreeWidgetItem([label + (f'\n"{shorten(detail, 34)}"' if detail else "")])
                child.setIcon(0, icons.icon(icon))
                child.setData(0, Qt.UserRole, ref)
                child.setToolTip(0, detail or label)
                top.addChild(child)
                self.add_discard(child, [ref], "Discard this edit")
            top.setExpanded(True)

    def add_discard(self, item, refs, tip):
        button = QToolButton()
        button.setAutoRaise(True)
        button.setIconSize(QSize(14, 14))
        button.setToolTip(tip)
        button.setStyleSheet(DISCARD_STYLE)
        # deferred: discarding rebuilds the list, which deletes this button mid-click
        button.clicked.connect(lambda: QTimer.singleShot(0, lambda: self.discard_requested.emit(refs)))
        self.setItemWidget(item, 1, button)

    def show_discard(self, hovered):
        """Only the hovered row shows its discard icon (item widgets cannot stay hidden: the view re-shows them)."""
        icon = icons.icon("close")
        icon.addPixmap(icons.pixmap("close", "#ffffff"), QIcon.Active)
        for i in range(self.topLevelItemCount()):
            top = self.topLevelItem(i)
            for item in (top, *(top.child(j) for j in range(top.childCount()))):
                button = self.itemWidget(item, 1)
                if button:
                    button.setIcon(icon if item is hovered else QIcon())

    def scroll_to_group(self, ref):
        for i in range(self.topLevelItemCount()):
            top = self.topLevelItem(i)
            if top.data(0, Qt.UserRole) == ref:
                self.scrollToItem(top, QAbstractItemView.PositionAtTop)

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Leave:
            self.show_discard(None)
        return False


def shorten(text, limit):
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 3] + "..."

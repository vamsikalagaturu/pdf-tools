from pathlib import Path

from PySide6.QtCore import QRect, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QFontMetrics, QPainter, QPalette
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QStyle,
    QStyledItemDelegate,
    QVBoxLayout,
    QWidget,
)

from pdf_tools import APP_NAME
from pdf_tools.ui import icons
from pdf_tools.ui.util import app_icon

FOLDER_ROLE = Qt.UserRole + 1


class StartPage(QWidget):
    """Shown when no PDF is open: open, merge, recent files."""

    open_requested = Signal(str)
    workspace_requested = Signal()

    def __init__(self, settings):
        super().__init__()
        self.settings = settings
        box = QWidget()
        box.setMaximumWidth(620)
        col = QVBoxLayout(box)
        col.setSpacing(10)
        logo = QLabel()
        logo.setPixmap(app_icon().pixmap(88, 88))
        logo.setAlignment(Qt.AlignCenter)
        col.addWidget(logo)
        title = QLabel(APP_NAME)
        font = title.font()
        font.setPointSizeF(font.pointSizeF() * 1.8)
        font.setBold(True)
        title.setFont(font)
        title.setAlignment(Qt.AlignCenter)
        col.addWidget(title)
        subtitle = QLabel("View, fill, annotate, redact, sign and merge PDFs")
        subtitle.setAlignment(Qt.AlignCenter)
        subtitle.setEnabled(False)
        col.addWidget(subtitle)
        col.addSpacing(16)

        row = QHBoxLayout()
        row.setSpacing(12)
        for text, slot, name in (
            ("Open PDF...", lambda: self.open_requested.emit(""), "open"),
            ("Combine && split PDFs...", self.workspace_requested.emit, "merge"),
        ):
            button = QPushButton(text)
            icons.bind(button, name)
            button.setIconSize(QSize(20, 20))
            button.setMinimumHeight(44)
            button.setMinimumWidth(160)
            button.clicked.connect(slot)
            row.addWidget(button)
        col.addLayout(row)
        col.addSpacing(16)

        self.recent_label = QLabel("Recent")
        font = self.recent_label.font()
        font.setBold(True)
        self.recent_label.setFont(font)
        col.addWidget(self.recent_label)
        self.recent = QListWidget()
        self.recent.setItemDelegate(RecentDelegate(self.recent))
        self.recent.setFrameShape(QListWidget.NoFrame)
        self.recent.setStyleSheet("QListWidget { background: transparent; }")
        self.recent.setMouseTracking(True)
        self.recent.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.recent.setCursor(Qt.PointingHandCursor)
        self.recent.itemClicked.connect(lambda item: self.open_requested.emit(item.data(Qt.UserRole)))
        col.addWidget(self.recent)
        hint = QLabel("or drop a PDF anywhere in this window")
        hint.setAlignment(Qt.AlignCenter)
        hint.setEnabled(False)
        col.addWidget(hint)

        outer = QVBoxLayout(self)
        outer.addStretch(1)
        outer.addWidget(box, 0, Qt.AlignHCenter)
        outer.addStretch(2)
        self.refresh()

    def refresh(self):
        self.recent.clear()
        home = str(Path.home())
        for path in self.settings.get("recent_files"):
            p = Path(path)
            if p.exists():
                item = QListWidgetItem(p.name)
                item.setData(Qt.UserRole, str(p))
                item.setData(FOLDER_ROLE, str(p.parent).replace(home, "~", 1))
                item.setToolTip(str(p))
                self.recent.addItem(item)
        count = self.recent.count()
        self.recent_label.setVisible(count > 0)
        self.recent.setVisible(count > 0)
        if count:
            self.recent.setFixedHeight(min(count, 8) * RecentDelegate.ROW_HEIGHT + 2)


class RecentDelegate(QStyledItemDelegate):
    """Two-line recent-file row: file icon, name, dimmed folder underneath; rounded hover highlight."""

    ROW_HEIGHT = 48

    def sizeHint(self, option, index):
        return QSize(option.rect.width(), self.ROW_HEIGHT)

    def paint(self, p, option, index):
        p.save()
        p.setRenderHint(QPainter.Antialiasing)
        r = option.rect.adjusted(2, 2, -2, -2)
        palette = option.palette
        if option.state & (QStyle.State_MouseOver | QStyle.State_Selected):
            hover = QColor(palette.color(QPalette.Highlight))
            hover.setAlpha(60)
            p.setBrush(hover)
            p.setPen(Qt.NoPen)
            p.drawRoundedRect(r, 6, 6)
        icon_size = 22
        icons.icon("file").paint(p, QRect(r.left() + 10, r.center().y() - icon_size // 2, icon_size, icon_size))
        text_left = r.left() + 10 + icon_size + 12
        width = r.right() - text_left - 10
        name_font = QFont(option.font)
        name_font.setBold(True)
        p.setFont(name_font)
        p.setPen(palette.color(QPalette.Text))
        name = QFontMetrics(name_font).elidedText(index.data(Qt.DisplayRole), Qt.ElideMiddle, width)
        p.drawText(QRect(text_left, r.top() + 5, width, r.height() // 2), Qt.AlignLeft | Qt.AlignVCenter, name)
        folder_font = QFont(option.font)
        folder_font.setPointSizeF(folder_font.pointSizeF() * 0.9)
        p.setFont(folder_font)
        p.setPen(palette.color(QPalette.Disabled, QPalette.Text))
        folder = QFontMetrics(folder_font).elidedText(index.data(FOLDER_ROLE), Qt.ElideMiddle, width)
        p.drawText(QRect(text_left, r.center().y(), width, r.height() // 2 - 5), Qt.AlignLeft | Qt.AlignVCenter, folder)
        p.restore()

from importlib.resources import files

from PySide6.QtCore import QBuffer, QByteArray, QIODevice
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPixmap


def logo_path():
    return files("pdf_tools") / "assets" / "logo.svg"


def app_icon():
    return QIcon(str(logo_path()))


def to_qimage(pix):
    fmt = QImage.Format_RGBA8888 if pix.alpha else QImage.Format_RGB888
    return QImage(pix.samples, pix.width, pix.height, pix.stride, fmt).copy()


def png_bytes(image):
    data = QByteArray()
    buf = QBuffer(data)
    buf.open(QIODevice.WriteOnly)
    image.save(buf, "PNG")
    return bytes(data)


def swatch_icon(rgb):
    pm = QPixmap(18, 18)
    pm.fill(QColor(*rgb))
    p = QPainter(pm)
    p.drawRect(0, 0, 17, 17)
    p.end()
    return QIcon(pm)


def hex_to_rgb(text):
    c = QColor(text)
    return (c.red(), c.green(), c.blue()) if c.isValid() else (255, 225, 0)


def rgb_to_hex(rgb):
    return QColor(*rgb).name()

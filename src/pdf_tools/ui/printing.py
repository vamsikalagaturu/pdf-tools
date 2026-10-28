"""Printing and print preview through Qt's print dialog; pages are sent as images at the printer's resolution."""

import pymupdf
from PySide6.QtCore import QRectF
from PySide6.QtGui import QPainter, QTransform
from PySide6.QtPrintSupport import QAbstractPrintDialog, QPrintDialog, QPrinter, QPrintPreviewDialog
from PySide6.QtWidgets import QDialog, QMessageBox

from pdf_tools.ui.util import to_qimage

PRINT_DPI = 300


def print_pdf(parent, render, pages, current, name, preview=False):
    """Print (or preview) the PDF bytes `render()` returns; `pages` is its page count, `current` a 0-based page."""
    printer = QPrinter(QPrinter.HighResolution)
    printer.setDocName(name)
    if preview:
        dlg = QPrintPreviewDialog(printer, parent)
        dlg.setWindowTitle(f"Print preview - {name}")
        dlg.paintRequested.connect(lambda p: paint_pages(parent, p, render(), current))
        dlg.exec()
        return
    dlg = QPrintDialog(printer, parent)
    dlg.setMinMax(1, pages)
    dlg.setOptions(
        dlg.options() | QAbstractPrintDialog.PrintPageRange | QAbstractPrintDialog.PrintCurrentPage
        | QAbstractPrintDialog.PrintCollateCopies
    )
    if dlg.exec() == QDialog.Accepted:
        paint_pages(parent, printer, render(), current)


def paint_pages(parent, printer, data, current):
    """Paint each chosen page as an image at the printer's resolution, fitted (and turned if needed) to the paper."""
    doc = pymupdf.open("pdf", data)
    n = len(doc)
    if printer.printRange() == QPrinter.PageRange:
        pages = list(range(max(printer.fromPage(), 1) - 1, min(printer.toPage(), n)))
    elif printer.printRange() == QPrinter.CurrentPage:
        pages = [min(current, n - 1)]
    else:
        pages = list(range(n))
    if printer.pageOrder() == QPrinter.LastPageFirst:
        pages.reverse()
    dpi = min(printer.resolution(), PRINT_DPI)
    area = QRectF(printer.pageLayout().paintRectPixels(printer.resolution()))
    painter = QPainter()
    if not painter.begin(printer):
        doc.close()
        QMessageBox.critical(parent, "Print", "Could not start printing.")
        return
    for i, p in enumerate(pages):
        if i:
            printer.newPage()
        image = to_qimage(doc[p].get_pixmap(dpi=dpi, alpha=False))
        if (image.width() > image.height()) != (area.width() > area.height()):
            image = image.transformed(QTransform().rotate(90))
        scale = min(area.width() / image.width(), area.height() / image.height())
        w, h = image.width() * scale, image.height() * scale
        painter.drawImage(QRectF((area.width() - w) / 2, (area.height() - h) / 2, w, h), image)
    painter.end()
    doc.close()

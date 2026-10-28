"""PDF Tools: view, redact, fill, annotate, sign and merge PDFs."""

from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("pdf-tools")
except PackageNotFoundError:
    __version__ = "unknown"
APP_NAME = "PDF Tools"
REPO_URL = "https://github.com/vamsikalagaturu/pdf-tools"

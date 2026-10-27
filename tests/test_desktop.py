from pdf_tools.core import desktop


def test_install_writes_entry_with_absolute_exec_and_uninstall_removes_it(tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    monkeypatch.setattr(desktop, "executable", lambda: "/opt/my tools/pdf-tools")
    entry, icon = desktop.install()
    assert entry == tmp_path / "applications" / "pdf-tools.desktop"
    assert 'Exec="/opt/my tools/pdf-tools" %F' in entry.read_text()
    assert icon.read_text().lstrip().startswith("<svg")
    assert desktop.uninstall() == [entry, icon]
    assert not entry.exists() and not icon.exists()
    assert desktop.uninstall() == []

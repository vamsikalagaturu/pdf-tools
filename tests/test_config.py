import tomllib

from pdf_tools.core.config import SETTINGS, Settings, migrate_legacy


def test_first_run_writes_commented_defaults(tmp_path):
    path = tmp_path / "settings.toml"
    s = Settings(path)
    text = path.read_text()
    assert tomllib.loads(text) == {k: d for k, (d, _) in SETTINGS.items()}
    assert "# dark | light" in text
    assert s.get("theme") == "dark"


def test_set_persists_and_reloads(tmp_path):
    path = tmp_path / "settings.toml"
    Settings(path).set("view_mode", "single")
    assert Settings(path).get("view_mode") == "single"


def test_bad_values_and_unknown_keys_fall_back(tmp_path):
    path = tmp_path / "settings.toml"
    path.write_text('theme = 3\nexport_dpi = 300\nbogus = "x"\nredact_auto_fill = "yes"\n')
    s = Settings(path)
    assert s.get("theme") == "dark"
    assert s.get("export_dpi") == 300
    assert s.get("redact_auto_fill") is True


def test_broken_file_is_replaced_with_defaults(tmp_path):
    path = tmp_path / "settings.toml"
    path.write_text("theme = [unclosed")
    assert Settings(path).get("theme") == "dark"
    assert tomllib.loads(path.read_text())["theme"] == "dark"


def test_quotes_survive_round_trip(tmp_path):
    path = tmp_path / "settings.toml"
    Settings(path).set("last_folder", 'C:\\a "b"')
    assert Settings(path).get("last_folder") == 'C:\\a "b"'


def test_legacy_signatures_copied_once(tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path))
    old = tmp_path / "pdf-redact" / "signatures"
    old.mkdir(parents=True)
    (old / "sig.png").write_bytes(b"png")
    migrate_legacy()
    assert (tmp_path / "pdf-tools" / "signatures" / "sig.png").read_bytes() == b"png"
    assert (old / "sig.png").exists()

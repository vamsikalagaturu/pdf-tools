import pytest

from pdf_tools.core.ranges import format_ranges, parse_ranges


def test_parse_mixed():
    assert parse_ranges("1-3, 7,5", 10) == [0, 1, 2, 4, 6]


def test_parse_dedupes_and_sorts():
    assert parse_ranges("3,1,1-2", 5) == [0, 1, 2]


@pytest.mark.parametrize("text", ["0", "4-2", "11", "x"])
def test_parse_rejects(text):
    with pytest.raises(ValueError):
        parse_ranges(text, 10)


def test_format_round_trip():
    pages = [0, 1, 2, 4, 6, 7]
    assert format_ranges(pages) == "1-3,5,7-8"
    assert parse_ranges(format_ranges(pages), 10) == pages

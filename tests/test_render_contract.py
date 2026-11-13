from pathlib import Path
import re


def test_video_paint_commits_one_offscreen_frame():
    source = Path(__file__).parents[1] / "src" / "gui.c"
    text = source.read_text(encoding="utf-8")
    match = re.search(r"LRESULT CALLBACK video_proc\(.*?\n\}", text, re.S)
    assert match, "video_proc must remain a self-contained paint path"
    body = match.group(0)
    assert "CreateCompatibleDC" in body
    assert "BitBlt" in body
    assert "StretchDIBits" in body


def test_layout_does_not_force_video_repaint_on_every_timer_tick():
    source = Path(__file__).parents[1] / "src" / "gui.c"
    text = source.read_text(encoding="utf-8")
    assert "MoveWindow(g_video, vi.left, vi.top, rw(vi), rh(vi), FALSE)" in text


def test_capture_handles_media_type_changes_and_stream_ticks():
    source = Path(__file__).parents[1] / "src" / "capture.c"
    text = source.read_text(encoding="utf-8")
    assert "MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED" in text
    assert "MF_SOURCE_READERF_STREAMTICK" in text


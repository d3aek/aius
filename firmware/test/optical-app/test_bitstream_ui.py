#!/usr/bin/env python3
"""Structural proof that every SD UI names the codec-agnostic bitstream seam."""

from pathlib import Path


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (FIRMWARE_ROOT / relative).read_text(encoding="utf-8")


def require(source: str, fragment: str, path: str) -> None:
    assert fragment in source, f"{path}: missing {fragment!r}"


def reject(source: str, fragment: str, path: str) -> None:
    assert fragment not in source, f"{path}: stale DTS-only label {fragment!r}"


def main() -> None:
    status_path = "sd/www/status.js"
    editor_path = "sd/www/dsp-editor.js"
    status_page_path = "sd/www/status.html"
    index_path = "sd/www/index.html"
    firmware_path = "src/optical_main.cpp"
    status = read(status_path)
    editor = read(editor_path)
    status_page = read(status_page_path)
    index = read(index_path)
    firmware = read(firmware_path)

    # Phase 4x moved the audio-path render from status.js into dsp-editor.js.
    path, source = editor_path, editor
    require(source, "pipeline.codec_pc", path)
    require(source, "codecPc === 1", path)
    require(source, "codecPc === 11 || codecPc === 12 || codecPc === 13", path)
    require(source, "codecPc === 21", path)
    require(source, "return 'AC-3'", path)
    require(source, "return 'DTS'", path)
    require(source, "return 'E-AC-3'", path)
    require(source, "return 'unknown'", path)
    require(source, "`bitstream (${codecName(status)})`", path)

    require(editor, "$('audio-state').textContent = audioStateText(status)", editor_path)
    require(editor, "$('format').textContent = streamFormatText(status)", editor_path)
    require(editor, "const encoding = carrierEncoding(status)", editor_path)
    require(index, 'id="audio-state"', index_path)
    require(index, 'id="format"', index_path)
    require(
        firmware,
        "dts_metrics.arena_high_water > ac3_metrics.arena_high_water",
        firmware_path,
    )
    require(
        firmware,
        "eac3_metrics.arena_high_water > shared_arena_high_water",
        firmware_path,
    )
    require(
        firmware,
        "(unsigned long)shared_arena_high_water",
        firmware_path,
    )
    require(firmware, '\\"eac3_decode_cycles\\":%llu', firmware_path)
    require(firmware, '\\"eac3_alloc_count\\":%lu', firmware_path)

    stale_labels = (
        "4: 'DTS'",
        "2: 'DTS (IEC 61937)'",
        "DTS framing and decoding cannot be bypassed",
        "same DTS, DSP, limiter, and meter path",
    )
    for source, path in (
        (status, status_path),
        (editor, editor_path),
        (status_page, status_page_path),
        (index, index_path),
    ):
        for stale in stale_labels:
            reject(source, stale, path)

    print("optical-app bitstream UI oracle: all tests passed")


if __name__ == "__main__":
    main()

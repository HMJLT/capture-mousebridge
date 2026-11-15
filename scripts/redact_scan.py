from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


NETWORK_PATTERNS = (
    re.compile(r"(?<!\d)(?:10|192\.168)\.\d{1,3}\.\d{1,3}\.\d{1,3}(?!\d)"),
    re.compile(r"(?<!\d)172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}(?!\d)"),
    re.compile(r"\b0\.0\.0\.0\b"),
    re.compile(r"(?i)(?:li@|khinggan_ed25519|authorized_keys|/home/li\b|C:\\Users\\HUMU)"),
)
SECRET_PATTERNS = (re.compile(r"(?i)(?:li@|khinggan_ed25519|authorized_keys|/home/li\b|C:\\Users\\HUMU)"),
                   re.compile(r"(?i)(?:api[_-]?key|access[_-]?token|private[_-]?key|password)\s*[:=]\s*['\"][^'\"]+"))

PRIVATE_EXTENSIONS = {".pt", ".pth", ".onnx", ".engine", ".bin", ".zip", ".rar", ".7z"}


def scan(root: Path, public: bool) -> list[str]:
    problems: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if any(part in {".git", "__pycache__", ".pytest_cache", "build"} for part in path.parts):
            continue
        if path.suffix.lower() in {".pyc", ".pyo"}:
            continue
        if path.name in {".gitignore"}:
            continue
        if path.name == "redact_scan.py":
            continue
        if public and path.suffix.lower() in PRIVATE_EXTENSIONS:
            problems.append(f"private artifact: {path.relative_to(root)}")
        if public and path.stat().st_size > 5 * 1024 * 1024:
            problems.append(f"oversized file: {path.relative_to(root)}")
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError as exc:
            problems.append(f"unreadable: {path.relative_to(root)} ({exc})")
            continue
        patterns = NETWORK_PATTERNS + SECRET_PATTERNS if public else SECRET_PATTERNS
        for pattern in patterns:
            if pattern.search(text):
                problems.append(f"{pattern.pattern}: {path.relative_to(root)}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--public", action="store_true")
    args = parser.parse_args()
    problems = scan(args.root.resolve(), args.public)
    if problems:
        print("\n".join(problems))
        return 1
    print(f"clean: {args.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


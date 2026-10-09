#!/usr/bin/env python3
"""Check repository-owned documentation links; no runtime execution."""
from __future__ import annotations

from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def documents() -> list[Path]:
    # Frozen import provenance and vendored upstream documentation are not rewritten.
    return sorted(set(ROOT.glob("*.md")) | set((ROOT / "doc").rglob("*.md")) |
                  set((ROOT / "graphics-translator").glob("*.md")))


def anchors(text: str) -> set[str]:
    found = set(re.findall(r'<a\s+(?:id|name)="([^"]+)"', text))
    counts: dict[str, int] = {}
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fenced = not fenced
        if fenced or not re.match(r"^#{1,6} +", line):
            continue
        title = re.sub(r"^#{1,6} +", "", line).rstrip(" #")
        title = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", title)
        slug = re.sub(r"[^\w -]", "", title.lower()).replace(" ", "-")
        number = counts.get(slug, 0)
        counts[slug] = number + 1
        found.add(slug + (f"-{number}" if number else ""))
    return found


class Documentation(unittest.TestCase):
    def test_agent_state_removed(self):
        self.assertFalse((ROOT / ".omo").exists())
        for path in documents():
            self.assertNotIn("](" + "../.omo/", path.read_text(), str(path))

    def test_local_links(self):
        failures: list[str] = []
        evidence_missing = 0
        checked = 0
        for path in documents():
            text = path.read_text()
            for match in re.finditer(r"\]\(([^)\s]+)\)", text):
                link = unquote(match.group(1))
                split = urlsplit(link)
                if split.scheme or split.netloc or link.startswith(("/", "$", "<")):
                    continue
                target = (path.parent / split.path).resolve() if split.path else path
                location = f"{path.relative_to(ROOT)}:{text.count(chr(10), 0, match.start()) + 1}"
                if target.is_relative_to(ROOT / "build"):
                    evidence_missing += not target.exists()
                    continue  # Evidence may be intentionally retired; source/doc links may not.
                if not target.exists():
                    failures.append(f"{location}: missing {link}")
                elif split.fragment and target.suffix == ".md":
                    if split.fragment not in anchors(target.read_text()):
                        failures.append(f"{location}: missing anchor {link}")
                checked += 1
        print(f"DOCUMENTATION_LINKS checked={checked} unavailable_local_evidence={evidence_missing}")
        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main(verbosity=2)

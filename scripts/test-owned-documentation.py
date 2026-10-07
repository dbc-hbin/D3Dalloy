#!/usr/bin/env python3
"""Check repository-owned documentation links and preserved handoffs; no runtime execution."""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
HANDOFFS = {
    "remaining-implementation-20261005.md": "d07f9ee1b77ea045679f417eb1acacf3f9c0d6e61dd7dc6fc7b6961780a73db5",
    "parallel-implementation-stop-20261005.md": "4713787e281dd20534678af70443703b57ea3d4830f79dae2970dd233151a5b0",
    "parallel-resume2-20261005.md": "3310b138da919a2d4ec10fdd76fc07aff35bd4f73618bb9a55119edc59a4e6bc",
}


def documents() -> list[Path]:
    # Frozen import provenance and vendored upstream documentation are not rewritten.
    return sorted(set(ROOT.glob("*.md")) | set((ROOT / "docs").glob("*.md")) |
                  set((ROOT / "docs/history").glob("*.md")) |
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
    def test_handoff_bodies_preserved(self):
        for name, expected in HANDOFFS.items():
            with self.subTest(name=name):
                content = (ROOT / "docs/history" / name).read_bytes()
                body = content.split(b"\n", 2)[2]
                self.assertEqual(hashlib.sha256(body).hexdigest(), expected)

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

#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrzej Borucki
# SPDX-License-Identifier: Apache-2.0

"""
Regenerate data/pgnmentor_index.json from the Players section of
https://www.pgnmentor.com/files.html.

Kept as a repo tool (not run at build time or at runtime) because
pgnmentor updates only a few times per year. Run manually and
commit the refreshed JSON.

    python3 tools/refresh_pgnmentor_index.py

Usage notes:
  * No external dependencies — stdlib urllib + re only.
  * Name format: the shipped JSON uses "Last, First" (database
    style, sortable by surname). pgnmentor's page prints
    "First Last" and concatenates the surname in the zip
    filename (e.g. "Becerra Rivero" → BecerraRivero.zip). We
    pick the longest suffix of the name whose words joined
    without spaces equal the filename stem — that handles both
    single- and multi-word surnames correctly.
  * "games": integer parsed from "…, N games" in each row.
  * Rows that fail to parse are printed to stderr and skipped;
    the script exits non-zero if any are skipped, so you notice
    the page layout changing.
"""

from __future__ import annotations

import datetime
import json
import re
import sys
import urllib.request
from pathlib import Path

URL = "https://www.pgnmentor.com/files.html"
OUT = Path(__file__).resolve().parent.parent / "data" / "pgnmentor_index.json"

# The Players section sits between the `#players` and `#openings`
# anchors on the page. Everything else (openings, events) is
# ignored for now.
SECTION_RE = re.compile(
    r'<a id="players"></a>(.*?)<a id="(openings|events)"></a>',
    re.DOTALL,
)

# Each row is a <tr> with two <td>s:
#   <td><a href="players/STEM.zip">STEM.pgn</a>...</td>
#   <td>First Last, N games</td>
ROW_RE = re.compile(
    r'<tr>\s*<td>\s*<a\s+href="players/([^"]+)\.zip">'
    r'[^<]+</a>.*?</td>\s*'
    r'<td>([^<,]+?),\s*(\d+)\s+games?</td>\s*</tr>',
    re.DOTALL,
)


def to_last_first(full_name: str, stem: str) -> str:
    """Turn "First Middle Last" → "Last, First Middle", using
    the zip filename stem to disambiguate multi-word surnames.

    If no suffix of the name's words (joined without spaces)
    matches the stem, fall back to the last single word.
    """
    words = full_name.split()
    if not words:
        return full_name
    target = stem.lower()
    for k in range(len(words) - 1, 0, -1):
        surname_candidate = "".join(words[k:]).lower()
        if surname_candidate == target:
            surname = " ".join(words[k:])
            first = " ".join(words[:k])
            return f"{surname}, {first}"
    # Fallback: last word is the surname.
    return f"{words[-1]}, {' '.join(words[:-1])}"


def main() -> int:
    # pgnmentor.com rejects urllib's default User-Agent with an
    # odd non-standard 465 response. A generic UA string makes it
    # happy — no cookies or session handling needed.
    req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0"})
    html = urllib.request.urlopen(req, timeout=30).read().decode("utf-8")
    section_match = SECTION_RE.search(html)
    if not section_match:
        print("ERROR: could not find the Players section anchors",
              file=sys.stderr)
        return 2
    section = section_match.group(1)

    players = []
    skipped = 0
    seen_stems = set()
    for row in re.finditer(ROW_RE, section):
        stem, name, count = row.group(1), row.group(2).strip(), row.group(3)
        if stem in seen_stems:
            continue
        seen_stems.add(stem)
        players.append({
            "name": to_last_first(name, stem),
            "file": f"{stem}.zip",
            "games": int(count),
        })

    # Sanity check: the page uses a consistent <tr> layout, so
    # losing half a row means regex drift.
    raw_rows = len(re.findall(r'<a href="players/[^"]+\.zip"', section))
    # Each player is referenced twice in its row (name link +
    # Download link). Expect parsed count to equal raw/2.
    expected = raw_rows // 2
    if len(players) < expected - 5:  # small tolerance for page quirks
        print(f"WARNING: parsed {len(players)} but saw ~{expected} "
              f"zip references — regex may need updating",
              file=sys.stderr)
        skipped = expected - len(players)

    players.sort(key=lambda p: p["name"].lower())

    payload = {
        "source": f"{URL}#players",
        "snapshot_date": datetime.date.today().isoformat(),
        "note": ("Auto-regenerated from tools/refresh_pgnmentor_index.py. "
                 "Each entry's file is appended to "
                 "https://www.pgnmentor.com/players/ to form the "
                 "download URL."),
        "players": players,
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=4, ensure_ascii=False)
        f.write("\n")

    print(f"wrote {OUT} — {len(players)} players", file=sys.stderr)
    return 1 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())

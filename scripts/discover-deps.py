#!/usr/bin/env python3
"""Fill in the missing "repo" fields in scripts/dependencies.json.

Six of the headers-only dependencies are published on qpackages.com without a
GitHub download URL, so their source repository cannot be derived from
qpm.shared.json the way the rest can. Every package does, however, record its
own repository in the qpm.json at the root of its extracted headers -- so an
extern/ tree that a previous `qpm restore` already produced holds the answer.

Run this once against such a tree:

    python3 scripts/discover-deps.py            # reads ./extern
    python3 scripts/discover-deps.py --extern /path/to/other/extern

After that, scripts/dependencies.json is complete and qpackages.com is never
needed again -- commit it.
"""

import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE / "dependencies.json"


def repo_from_url(url: str) -> str | None:
    match = re.match(r"(?:https?://)?(?:www\.)?github\.com/([^/]+)/([^/.]+)", url.strip())
    if not match:
        return None
    return f"{match.group(1)}/{match.group(2)}"


def find_repo(extern: pathlib.Path, dep_id: str) -> str | None:
    """Look for <extern>/includes/<id>/qpm.json and read its info.url."""
    candidates = [
        extern / "includes" / dep_id / "qpm.json",
        extern / "includes" / dep_id / "qpm.shared.json",
    ]
    # Some layouts nest the package one level deeper.
    candidates += sorted((extern / "includes" / dep_id).glob("*/qpm.json"))
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            data = json.loads(candidate.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        url = (data.get("config", data).get("info", {}) or {}).get("url", "")
        repo = repo_from_url(url) if url else None
        if repo:
            return repo
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--extern", default="extern", help="path to an existing extern/ tree (default: ./extern)")
    args = parser.parse_args()

    extern = pathlib.Path(args.extern).resolve()
    if not (extern / "includes").is_dir():
        print(f"error: {extern}/includes does not exist.", file=sys.stderr)
        print("Point --extern at a tree a previous `qpm restore` produced.", file=sys.stderr)
        return 1

    manifest = json.loads(MANIFEST.read_text())
    missing = [d for d in manifest["dependencies"] if not d.get("repo")]
    if not missing:
        print("Nothing to discover: every dependency already names a repository.")
        return 0

    found = 0
    for dep in missing:
        repo = find_repo(extern, dep["id"])
        if repo:
            dep["repo"] = repo
            found += 1
            if not dep.get("ref"):
                # qpm publishes header refs as version/vMAJOR_MINOR_PATCH for
                # most packages. It is only a default -- restore-deps.py fails
                # loudly with a 404 if a package uses a different scheme, and
                # the ref can then be corrected by hand.
                dep["ref"] = "version/v" + dep["version"].replace(".", "_")
                print(f"  {dep['id']:<26} -> {repo} (ref guessed: {dep['ref']})")
            else:
                print(f"  {dep['id']:<26} -> {repo}")
        else:
            print(f"  {dep['id']:<26} -- not found under {extern}/includes")

    if found:
        MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"\nUpdated {MANIFEST} ({found}/{len(missing)} resolved). Commit it.")

    remaining = [d["id"] for d in manifest["dependencies"] if not d.get("repo")]
    if remaining:
        print(f"\nStill unresolved: {', '.join(remaining)}", file=sys.stderr)
        print("Add their GitHub repo by hand (owner/name) and re-run restore-deps.py.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

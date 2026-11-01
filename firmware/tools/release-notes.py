#!/usr/bin/env python3
"""Release notes for a tag, built from the commit messages since the previous tag.

Usage: release-notes.py <tag>      (run inside the repository; needs the tags and history)
A stable tag lists everything since the previous stable tag; a beta tag lists what changed since the previous tag of any kind.
Commits follow the project format: summary lines "CATEGORY: text", a blank line, then one plain-language paragraph per summary line.
"""
import re
import subprocess
import sys

SECTIONS = [("FEATURE", "New"), ("BUG", "Fixed"), ("SECURITY", "Security"), ("CHORE", "Maintenance"), ("REFACTOR", "Maintenance")]
SUMMARY = re.compile(r"^(FEATURE|BUG|SECURITY|REFACTOR|CHORE):\s*(.+)$")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def previous(tag):
    stable = "-beta" not in tag
    tags = [t for t in git("tag", "--list", "v*", "--sort=-v:refname").split() if not stable or "-beta" not in t]
    return tags[tags.index(tag) + 1] if tag in tags and tags.index(tag) + 1 < len(tags) else None


def entries(rev_range):
    found = {}
    for message in git("log", "--no-merges", "--format=%B%x00", rev_range).split("\0"):
        blocks = [b.strip() for b in re.split(r"\n\s*\n", message.strip()) if b.strip()]
        if not blocks:
            continue
        lines = [SUMMARY.match(line.strip()) for line in blocks[0].splitlines()]
        if not all(lines):
            continue
        paragraphs = blocks[1:]
        for i, m in enumerate(lines):
            text = " ".join(paragraphs[i].split()) if len(paragraphs) == len(lines) else ""
            found.setdefault(m.group(1), []).append((m.group(2).strip(), text))
    return found


def main():
    tag = sys.argv[1]
    before = previous(tag)
    found = entries(f"{before}..{tag}" if before else tag)
    out = []
    for heading in dict.fromkeys(h for _, h in SECTIONS):
        items = [e for cat, h in SECTIONS if h == heading for e in found.get(cat, [])]
        if not items:
            continue
        out.append(f"## {heading}\n")
        for summary, text in items:
            out.append(f"- **{summary}**" + (f"  \n  {text}" if text else ""))
        out.append("")
    if not out:
        out = ["No user-visible changes."]
    if before:
        out.append(f"Changes since {before}.")
    print("\n".join(out))


main()

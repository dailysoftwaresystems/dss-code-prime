#!/usr/bin/env python3
"""Render a packaging template by replacing @NAME@ placeholders with env vars.

Usage:  python packaging/render.py <template> <output>

Every `@NAME@` token in <template> is replaced with the value of the
environment variable NAME. A placeholder whose variable is unset is a FATAL
error (fail-loud) — a manifest silently missing its version or sha256 would
ship a broken package. Keeps the manifests declarative and free of shell
`${...}` clashes (Ruby/JSON/YAML all use `${}` themselves).
"""
import os
import re
import sys

if len(sys.argv) != 3:
    sys.exit("usage: render.py <template> <output>")

template_path, output_path = sys.argv[1], sys.argv[2]
with open(template_path, "r", encoding="utf-8") as fh:
    text = fh.read()

missing = sorted({m for m in re.findall(r"@([A-Z0-9_]+)@", text) if m not in os.environ})
if missing:
    sys.exit(f"render.py: {template_path}: unset placeholder(s): {', '.join('@'+m+'@' for m in missing)}")


def _sub(match: "re.Match[str]") -> str:
    return os.environ[match.group(1)]


rendered = re.sub(r"@([A-Z0-9_]+)@", _sub, text)
with open(output_path, "w", encoding="utf-8", newline="\n") as fh:
    fh.write(rendered)
print(f"rendered {template_path} -> {output_path}")

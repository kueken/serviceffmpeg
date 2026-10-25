#!/usr/bin/env python3
"""
AI-Autofix für C/C++-Code.
- Arbeitet auf dem ausgecheckten Branch (test).
- Sammelt C/C++-Quellen, lässt DeepSeek pro Datei einen unified diff erzeugen.
- Verifiziert den Patch mit `git apply --check`, bevor er übernommen wird.
- Schreibt die Anzahl geänderter Dateien nach /tmp/ai_fix_changed.
"""
import os, sys, subprocess, json, re
from pathlib import Path
import requests

API   = "https://api.deepseek.com/chat/completions"
KEY   = os.environ["DEEPSEEK_API_KEY"]
MODEL = "deepseek-chat"

# --- an dein Projekt anpassen ---
TARGETS = ["."]                       # Wurzel; Excludes unten
EXTS    = {".cpp", ".c", ".h", ".hpp", ".cc"}
EXCLUDE_DIRS = {".git", "build", "tmp", "node_modules", ".github"}
EXCLUDE_FILES = {
    "bcm_ioctls.h",       # Kernel-ABI, nicht anfassen
    "config.h",           # generiert
    "version.h",          # generiert
}
MAX_CHARS = 80000                     # pro Datei; darüber: Datei überspringen
MAX_FILES = 40                        # Kostenbremse pro Lauf

SYSTEM_PROMPT = """\
Du bist ein Senior-Entwickler für C++ (Enigma2-Plugins, FFmpeg, DVB-API).
Aufgabe: Finde und behebe NUR offensichtliche Bugs, Ressourcenlecks,
off-by-one-Fehler, fehlende Fehlerprüfungen und klare Performance-Probleme.

REGELN:
- Ändere die öffentliche API nicht.
- Ändere keine #include-Reihenfolgen, außer wenn ein Include fehlt.
- Entferne keine Kommentare, die erklären, warum etwas so ist.
- Keine reinen Stiländerungen (Whitespace, Umbenennungen).
- Wenn du nichts Sinnvolles findest: antworte exakt mit NO_CHANGES.
- Sonst: antworte NUR mit einem unified diff im Format
  `--- a/<pfad>\\n+++ b/<pfad>\\n@@ ... @@\\n...`,
  ohne Markdown-Fences, ohne Erklärtext.
"""

def ask_diff(path: Path, code: str) -> str:
    user = f"Datei: {path}\n\n```\n{code}\n```\n"
    r = requests.post(
        API,
        headers={"Authorization": f"Bearer {KEY}"},
        json={
            "model": MODEL,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user",   "content": user},
            ],
            "temperature": 0.1,
            "max_tokens": 8192,
        },
        timeout=180,
    )
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"].strip()

def strip_fences(s: str) -> str:
    s = re.sub(r"^```(?:diff)?\s*\n", "", s)
    s = re.sub(r"\n```\s*$", "", s)
    return s

def try_apply(patch: str) -> bool:
    """Patch gegen Arbeitskopie prüfen und anwenden."""
    p = subprocess.run(
        ["git", "apply", "--check", "--whitespace=nowarn", "-"],
        input=patch, text=True, capture_output=True,
    )
    if p.returncode != 0:
        print(f"  patch rejected: {p.stderr.strip().splitlines()[:3]}")
        return False
    subprocess.run(
        ["git", "apply", "--whitespace=nowarn", "-"],
        input=patch, text=True, check=True,
    )
    return True

def iter_sources():
    seen = 0
    for root in TARGETS:
        for p in Path(root).rglob("*"):
            if seen >= MAX_FILES:
                return
            if not p.is_file():
                continue
            if any(part in EXCLUDE_DIRS for part in p.parts):
                continue
            if p.suffix not in EXTS:
                continue
            yield p
            seen += 1

def main() -> int:
    changed = 0
    for p in iter_sources():
        try:
            code = p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            print(f"skip (binary): {p}")
            continue
        if len(code) > MAX_CHARS:
            print(f"skip (too large, {len(code)} chars): {p}")
            continue

        print(f"analyzing: {p}")
        try:
            answer = strip_fences(ask_diff(p, code))
        except Exception as e:
            print(f"  API error: {e}")
            continue

        if not answer or answer == "NO_CHANGES":
            print("  no changes")
            continue

        if try_apply(answer):
            changed += 1
            print(f"  applied ({len(answer)} chars diff)")

    Path("/tmp/ai_fix_changed").write_text(str(changed))
    print(f"files changed: {changed}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

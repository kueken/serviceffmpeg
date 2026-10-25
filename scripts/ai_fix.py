#!/usr/bin/env python3
"""
AI-Autofix für C/C++-Code (Enigma2-Plugin serviceffmpeg).

Ablauf:
  1. Sammelt C/C++-Quellen (TARGETS, EXTS), respektiert EXCLUDE_DIRS/EXCLUDE_FILES.
  2. Fragt DeepSeek pro Datei nach einem unified diff.
  3. Normalisiert die Antwort (Markdown-Fences, Erklärtext, Pfade).
  4. Prüft mit `git apply --check`; wendet nur an, wenn gültig.
  5. Schreibt Anzahl geänderter Dateien nach /tmp/ai_fix_changed.

Exit-Code ist immer 0 — der Workflow entscheidet anhand /tmp/ai_fix_changed.
"""

import os
import re
import sys
import subprocess
from pathlib import Path

import requests

API   = "https://api.deepseek.com/chat/completions"
KEY   = os.environ["DEEPSEEK_API_KEY"]
MODEL = "deepseek-chat"

# ---------------------------------------------------------------------------
# Projekt-Konfiguration
# ---------------------------------------------------------------------------
TARGETS = ["."]
EXTS = {".cpp", ".c", ".h", ".hpp", ".cc"}

EXCLUDE_DIRS = {".git", "build", "tmp", "node_modules", ".github", "__pycache__"}

# Dateien, die der Bot niemals anfassen darf
EXCLUDE_FILES = {
    "bcm_ioctls.h",   # Kernel-ABI, Enums und ioctl-Makros müssen exakt bleiben
    "config.h",       # generiert
    "version.h",      # generiert
}

MAX_CHARS = 80000     # pro Datei; darüber überspringen
MAX_FILES = 40        # Kostenbremse pro Lauf
BAD_PATCH_DIR = Path("/tmp/ai_fix_bad_patches")

# ---------------------------------------------------------------------------
# Prompt
# ---------------------------------------------------------------------------
SYSTEM_PROMPT = """\
Du bist ein Senior-Entwickler für C++ (Enigma2-Plugins, FFmpeg, DVB-API).

Aufgabe: Finde und behebe NUR offensichtliche Bugs, Ressourcenlecks,
off-by-one-Fehler und fehlende Fehlerprüfungen.

AUSGABEFORMAT — strikt einhalten:
- Antworte ausschließlich mit einem unified diff.
- Erste Zeile:  --- a/<exakter-pfad-aus-dem-user-prompt>
- Zweite Zeile: +++ b/<exakter-pfad-aus-dem-user-prompt>
- Danach Hunk-Header: @@ -start,count +start,count @@ mit korrekten Zeilennummern.
- Kontextzeilen beginnen mit einem Leerzeichen, Entfernungen mit '-', Ergänzungen mit '+'.
- Mindestens 3 Zeilen Kontext um jede Änderung.
- KEIN Markdown, KEINE ```-Fences, KEIN Erklärtext davor oder danach.
- Wenn du nichts Sinnvolles findest: antworte exakt mit NO_CHANGES.

REGELN:
- Ändere die öffentliche API nicht.
- Ändere keine #include-Reihenfolgen, außer wenn ein Include fehlt.
- Entferne keine Kommentare, die erklären, warum etwas so ist.
- Keine reinen Stiländerungen (Whitespace, Umbenennungen).
"""

# ---------------------------------------------------------------------------
# DeepSeek
# ---------------------------------------------------------------------------
def ask_diff(rel_path: str, code: str) -> str:
    user = f"Datei: {rel_path}\n\n```\n{code}\n```\n"
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

# ---------------------------------------------------------------------------
# Patch-Normalisierung
# ---------------------------------------------------------------------------
def normalize_patch(patch: str, rel_path: str) -> str:
    """
    - Entfernt Markdown-Fences.
    - Schneidet Erklärtext vor '--- ' ab.
    - Erzwingt korrekte a/ b/ Pfade.
    - Verwirft Text nach dem letzten Hunk.
    - Stellt sicher, dass die Datei mit Newline endet.
    """
    # Markdown-Fences
    patch = re.sub(r"^```(?:diff|patch)?\s*\n", "", patch, flags=re.MULTILINE)
    patch = re.sub(r"\n```\s*$", "", patch, flags=re.MULTILINE)

    # Alles vor dem ersten '--- ' abschneiden
    idx = patch.find("--- ")
    if idx > 0:
        patch = patch[idx:]
    if not patch.startswith("--- "):
        return ""

    lines = patch.splitlines()

    # Kopfzeilen normalisieren
    fixed_head = []
    for ln in lines[:2]:
        if ln.startswith("--- "):
            fixed_head.append(f"--- a/{rel_path}")
        elif ln.startswith("+++ "):
            fixed_head.append(f"+++ b/{rel_path}")
    if len(fixed_head) != 2:
        return ""

    # Hunks einsammeln, Trailing-Erklärtext verwerfen
    out = list(fixed_head)
    in_hunks = False
    for ln in lines[2:]:
        if ln.startswith("@@"):
            in_hunks = True
            out.append(ln)
            continue
        if not in_hunks:
            # Zwischen Kopf und erstem Hunk darf nur Leerraum stehen
            if ln.strip() == "":
                continue
            return ""
        if ln == "" or ln[0] in (" ", "+", "-", "\\"):
            out.append(ln)
        # sonst: Erklärtext → verwerfen

    if not any(l.startswith("@@") for l in out):
        return ""

    if out[-1] != "":
        out.append("")
    return "\n".join(out) + "\n"

# ---------------------------------------------------------------------------
# Patch anwenden
# ---------------------------------------------------------------------------
def try_apply(patch: str, rel_path: str, raw_answer: str) -> bool:
    patch = normalize_patch(patch, rel_path)
    if not patch:
        print("  no valid diff after normalization")
        _save_bad(raw_answer, rel_path)
        return False

    check = subprocess.run(
        ["git", "apply", "--check", "--whitespace=nowarn", "-"],
        input=patch, text=True, capture_output=True,
    )
    if check.returncode != 0:
        err = check.stderr.strip().splitlines()[:3]
        print(f"  patch rejected: {err}")
        _save_bad(raw_answer, rel_path)
        return False

    subprocess.run(
        ["git", "apply", "--whitespace=nowarn", "-"],
        input=patch, text=True, check=True,
    )
    return True

def _save_bad(raw: str, rel_path: str) -> None:
    BAD_PATCH_DIR.mkdir(parents=True, exist_ok=True)
    safe = rel_path.replace("/", "_").replace("\\", "_")
    (BAD_PATCH_DIR / f"{safe}.diff").write_text(raw, encoding="utf-8")

# ---------------------------------------------------------------------------
# Dateien sammeln
# ---------------------------------------------------------------------------
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
            if p.name in EXCLUDE_FILES:
                continue
            if p.suffix not in EXTS:
                continue
            yield p
            seen += 1

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> int:
    changed = 0
    BAD_PATCH_DIR.mkdir(parents=True, exist_ok=True)

    for p in iter_sources():
        try:
            code = p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            print(f"skip (binary): {p}")
            continue

        if len(code) > MAX_CHARS:
            print(f"skip (too large, {len(code)} chars): {p}")
            continue

        rel = p.as_posix()
        print(f"analyzing: {rel}")

        try:
            answer = ask_diff(rel, code)
        except Exception as e:
            print(f"  API error: {e}")
            continue

        if not answer or answer == "NO_CHANGES":
            print("  no changes")
            continue

        if try_apply(answer, rel, answer):
            changed += 1
            print(f"  applied ({len(answer)} chars diff)")

    Path("/tmp/ai_fix_changed").write_text(str(changed))
    print(f"files changed: {changed}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

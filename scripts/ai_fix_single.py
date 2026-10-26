#!/usr/bin/env python3
"""
AI-Autofix für eine einzelne Datei — Search/Replace-Ansatz.

Wird vom Matrix-Job aufgerufen: python ai_fix_single.py <datei>
Schreibt Ergebnis nach /tmp/ai_fix_result_<safe_name>.json.
"""

import json
import os
import re
import sys
from pathlib import Path

import requests
from patch_tool import parse_blocks, apply_edits, TextNotFoundError, AmbiguousMatchError

API = "https://api.deepseek.com/chat/completions"
KEY = os.environ["DEEPSEEK_API_KEY"]
MODEL = "deepseek-chat"

MAX_CHARS = 120000   # großzügig — Search/Replace braucht keine Zeilennummern

SEARCH_REPLACE_PROMPT = """\
Du bist ein Senior-Entwickler für C++ (Enigma2-Plugins, FFmpeg, DVB-API).

Aufgabe: Finde und behebe NUR offensichtliche Bugs, Ressourcenlecks,
off-by-one-Fehler und fehlende Fehlerprüfungen.

AUSGABEFORMAT — strikt einhalten. Für JEDE Änderung ein Block:

<<<<<<< SEARCH
[exakter alter Code, 3-5 Zeilen Kontext, so wie er in der Datei steht]
=======
[neuer Code]
>>>>>>> REPLACE

REGELN:
- Der SEARCH-Block muss EXAKT so in der Datei stehen (inkl. Einrückung).
- Mehrere Änderungen: mehrere Blöcke hintereinander.
- KEIN Markdown, KEINE ```-Fences, KEIN Erklärtext.
- Wenn du nichts findest: antworte exakt mit NO_CHANGES.
- Ändere keine #include-Reihenfolgen.
- Entferne keine Kommentare, die erklären, warum etwas so ist.
"""

SUMMARY_PROMPT = """\
Fasse in EINER Zeile (max. 80 Zeichen) zusammen, was dieser Diff ändert.
Keine Einleitung, keine Anführungszeichen, kein Punkt am Ende.
"""

def ask_edits(rel_path: str, code: str) -> str:
    user = f"Datei: {rel_path}\n\n```\n{code}\n```\n"
    r = requests.post(
        API,
        headers={"Authorization": f"Bearer {KEY}"},
        json={
            "model": MODEL,
            "messages": [
                {"role": "system", "content": SEARCH_REPLACE_PROMPT},
                {"role": "user",   "content": user},
            ],
            "temperature": 0.1,
            "max_tokens": 8192,
        },
        timeout=180,
    )
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"].strip()

def ask_summary(diff_text: str) -> str:
    try:
        r = requests.post(
            API,
            headers={"Authorization": f"Bearer {KEY}"},
            json={
                "model": MODEL,
                "messages": [
                    {"role": "system", "content": SUMMARY_PROMPT},
                    {"role": "user",   "content": diff_text[:4000]},
                ],
                "temperature": 0.1,
                "max_tokens": 64,
            },
            timeout=60,
        )
        r.raise_for_status()
        s = r.json()["choices"][0]["message"]["content"].strip()
        return s.splitlines()[0][:80]
    except Exception:
        return "automated code improvements"

def strip_fences(s: str) -> str:
    s = re.sub(r"^```(?:cpp|c\+\+|c|h)?\s*\n", "", s, flags=re.MULTILINE)
    s = re.sub(r"\n```\s*$", "", s, flags=re.MULTILINE)
    return s

def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: ai_fix_single.py <datei>")
        return 1

    target = Path(sys.argv[1])
    if not target.is_file():
        print(f"not a file: {target}")
        return 1

    try:
        old = target.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        print(f"skip (binary): {target}")
        return 0

    if len(old) > MAX_CHARS:
        print(f"skip (too large, {len(old)} chars): {target}")
        return 0

    rel = target.as_posix()
    print(f"analyzing: {rel}")

    try:
        answer = strip_fences(ask_edits(rel, old))
    except Exception as e:
        print(f"API error: {e}")
        return 1

    if not answer or answer == "NO_CHANGES":
        print("no changes")
        _write_result(rel, False, "")
        return 0

    # llm-patch-tool parst die Blöcke
    edits = parse_blocks(answer)
    if not edits:
        print("no valid SEARCH/REPLACE blocks found")
        _write_result(rel, False, "")
        return 0

    # Atomar anwenden — entweder alle oder keine
    try:
        result = apply_edits(target, edits)
    except (TextNotFoundError, AmbiguousMatchError) as e:
        print(f"rejected: {type(e).__name__}: {e}")
        _write_result(rel, False, "")
        return 0
    except Exception as e:
        print(f"rejected: {e}")
        _write_result(rel, False, "")
        return 0

    summary = ask_summary(result.diff)
    print(f"applied ({result.edits_applied} edits) — {summary}")
    _write_result(rel, True, summary)
    return 0

def _write_result(rel: str, changed: bool, summary: str) -> None:
    safe = rel.replace("/", "_").replace("\\", "_")
    out = Path(f"/tmp/ai_fix_result_{safe}.json")
    out.write_text(json.dumps({
        "file": rel,
        "changed": changed,
        "summary": summary,
    }, indent=2), encoding="utf-8")

if __name__ == "__main__":
    sys.exit(main())

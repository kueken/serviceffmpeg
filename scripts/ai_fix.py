import os, sys, subprocess, requests
from pathlib import Path

API = "https://api.deepseek.com/chat/completions"
KEY = os.environ["DEEPSEEK_API_KEY"]
TARGETS = ["src"]                      # <-- anpassen
EXTS = {".py", ".js", ".ts", ".cpp", ".h"}           # <-- anpassen
MAX_CHARS = 12000                      # pro Datei, Token-Limit

def ask(code: str) -> str:
    r = requests.post(API, headers={"Authorization": f"Bearer {KEY}"}, json={
        "model": "deepseek-chat",
        "messages": [
            {"role": "system", "content":
             "Du bist ein Senior-Entwickler. Optimiere den Code: Lesbarkeit, "
             "Performance, offensichtliche Bugs. Verändere die Funktionalität NICHT. "
             "Antworte NUR mit dem vollständigen neuen Dateiinhalt, ohne Markdown-Fences."},
            {"role": "user", "content": code},
        ],
        "temperature": 0.2,
    }, timeout=120)
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"].strip()

changed = 0
for root in TARGETS:
    for p in Path(root).rglob("*"):
        if p.suffix not in EXTS or not p.is_file():
            continue
        code = p.read_text(encoding="utf-8")
        if len(code) > MAX_CHARS:
            print(f"skip (too large): {p}")
            continue
        new = ask(code)
        if new and new != code:
            p.write_text(new + "\n", encoding="utf-8")
            changed += 1
            print(f"updated: {p}")

print(f"files changed: {changed}")
sys.exit(0 if changed else 1)   # exit 1 => kein Commit nötig

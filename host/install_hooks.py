#!/usr/bin/env python3
"""
Install (or remove) the ClaudeWatch hooks in ~/.claude/settings.json.

  python3 install_hooks.py            add hooks (idempotent, keeps existing hooks)
  python3 install_hooks.py --remove   remove only the ClaudeWatch entries
  python3 install_hooks.py --print    show the JSON that would be merged

A timestamped backup of settings.json is written before any change.
"""
import json
import os
import shutil
import sys
import time

SETTINGS = os.path.expanduser("~/.claude/settings.json")
HOOK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "claude_watch_hook.py")
MARKER = "claude_watch_hook.py"
COMMAND = f"python3 '{HOOK_SCRIPT}'"

# events that take a tool matcher -> "*" (all tools); others take no matcher
EVENTS = {
    "SessionStart": None,
    "UserPromptSubmit": None,
    "PreToolUse": "*",
    "PostToolUse": "*",
    "PermissionRequest": "*",
    "Notification": None,
    "Stop": None,
    "SessionEnd": None,
    "PreCompact": None,
}


def entry(matcher):
    e = {"hooks": [{"type": "command", "command": COMMAND, "timeout": 10}]}
    if matcher is not None:
        e["matcher"] = matcher
    return e


def is_ours(group):
    return any(MARKER in (h.get("command") or "") for h in group.get("hooks", []))


def main():
    remove = "--remove" in sys.argv
    if "--print" in sys.argv:
        print(json.dumps({"hooks": {ev: [entry(m)] for ev, m in EVENTS.items()}}, indent=2))
        return

    settings = {}
    if os.path.exists(SETTINGS):
        with open(SETTINGS) as f:
            settings = json.load(f)
        shutil.copy2(SETTINGS, SETTINGS + ".bak-" + time.strftime("%Y%m%d-%H%M%S"))
    hooks = settings.setdefault("hooks", {})

    changed = 0
    for ev, matcher in EVENTS.items():
        groups = hooks.get(ev, [])
        kept = [g for g in groups if not is_ours(g)]
        if not remove:
            kept.append(entry(matcher))
        if kept != groups:
            changed += 1
        if kept:
            hooks[ev] = kept
        else:
            hooks.pop(ev, None)
    if not hooks:
        settings.pop("hooks", None)

    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    with open(SETTINGS, "w") as f:
        json.dump(settings, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"{'removed' if remove else 'installed'} ClaudeWatch hooks in {SETTINGS} ({changed} events touched)")
    if not remove:
        print("hook script:", HOOK_SCRIPT)
        print("note: already-running Claude Code sessions pick hooks up on their next start.")


if __name__ == "__main__":
    main()

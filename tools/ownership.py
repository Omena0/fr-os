#!/usr/bin/env python3
"""
File ownership for agents working a shared tree.

Several agents edit one working tree at once. This is the arbiter for "who is
allowed to touch what", so it has to be safe under concurrency and it has to
say something useful when it says no.

    ./tools/ownership.py claim src/kernel/pmm.c --agent alice
    ./tools/ownership.py view
    ./tools/ownership.py check src/kernel/pmm.c --agent alice
    ./tools/ownership.py release src/kernel/pmm.c --agent alice
    ./tools/ownership.py clear --force
    ./tools/ownership.py import-legacy ownership.txt

Design notes
------------
*Concurrency.*  Every read-modify-write runs under an exclusive `flock` on a
sidecar lock file. `flock` is released by the kernel when the process exits,
including on a crash, so a killed agent cannot wedge the file. Within the lock
the store is re-read from disk, so two agents racing produce two serialised
operations rather than one lost update.

*Expiry.*  A claim carries a TTL (default 3h, overridable per claim). An expired
claim is treated as free everywhere: `view` marks it, `check` ignores it, and
`claim` may take it. Expiry exists because the alternative is a stale claim
blocking the work forever -- which is what happened earlier today when an agent
was killed mid-turn and left three claims behind. `--hours 0` means no claim
succeeds, which is a cheap way to test the tool.

*Paths.*  Normalised to repo-root-relative so `src/kernel/pmm.c`,
`./src/kernel/pmm.c` and an absolute path are one claim, not three.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import fcntl
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STORE = REPO / "ownership.json"
LOCK = REPO / "ownership.json.lock"

DEFAULT_HOURS = 3.0
# Names are free text but must be non-empty and reasonably short, so a claim
# that blocks someone is attributable to a person rather than a typo.
MAX_AGENT_LEN = 48


# --------------------------------------------------------------- time ------

def now() -> _dt.datetime:
    return _dt.datetime.now(_dt.timezone.utc)


def iso(t: _dt.datetime) -> str:
    return t.replace(microsecond=0).isoformat()


def parse(ts: str) -> _dt.datetime:
    try:
        d = _dt.datetime.fromisoformat(ts)
    except ValueError:
        return now() - _dt.timedelta(seconds=1)   # treat garbage as expired
    return d if d.tzinfo else d.replace(tzinfo=_dt.timezone.utc)


def human(delta: _dt.timedelta) -> str:
    s = int(delta.total_seconds())
    sign = "-" if s < 0 else ""
    s = abs(s)
    if s < 60:
        return f"{sign}{s}s"
    if s < 3600:
        return f"{sign}{s // 60}m"
    if s < 86400:
        return f"{sign}{s // 3600}h{(s % 3600) // 60:02d}m"
    return f"{sign}{s // 86400}d{(s % 86400) // 3600:02d}h"


# --------------------------------------------------------------- store -----

def empty() -> dict:
    return {"version": 1, "claims": {}}


def load() -> dict:
    if not STORE.exists():
        return empty()
    try:
        data = json.loads(STORE.read_text())
    except (OSError, json.JSONDecodeError) as e:
        die(f"cannot read {STORE.name}: {e}\n"
            f"       Delete it if it is corrupt: rm {STORE.name}")
    if not isinstance(data, dict) or "claims" not in data:
        die(f"{STORE.name} is malformed (no 'claims'). Remove it and retry.")
    return data


def save(data: dict) -> None:
    tmp = STORE.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, STORE)          # atomic on the same filesystem


class locked:
    """Exclusive advisory lock around a read-modify-write."""

    def __enter__(self):
        LOCK.parent.mkdir(parents=True, exist_ok=True)
        self.fh = open(LOCK, "a+")
        try:
            fcntl.flock(self.fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            die("another ownership operation is in flight; retry in a moment")
        return load()

    def __exit__(self, *exc):
        fcntl.flock(self.fh, fcntl.LOCK_UN)
        self.fh.close()
        return False


# --------------------------------------------------------------- helpers ---

def die(msg: str, code: int = 2) -> None:
    print(f"ownership: {msg}", file=sys.stderr)
    sys.exit(code)


def norm(path: str) -> str:
    p = Path(path)
    try:
        p = p.resolve().relative_to(REPO)
    except ValueError:
        die(f"{path!r} is not inside the repository ({REPO}).\n"
            f"       Claims are repo-relative; agents must work in this tree.")
    s = str(p)
    if not (REPO / s).exists():
        die(f"{s} does not exist. Check the path.")
    return s


def expired(claim: dict) -> bool:
    return parse(claim["expires"]) <= now()


def find(data: dict, path: str) -> dict | None:
    c = data["claims"].get(path)
    return c if (c and not expired(c)) else None


def show(path: str, c: dict | None) -> str:
    if not c:
        return "  free"
    return (f"  HELD BY {c['agent']}  for {human(parse(c['expires']) - now())} "
            f"(expires {c['expires']})")


# --------------------------------------------------------------- commands ---

def cmd_claim(a) -> int:
    path = norm(a.path)
    agent = a.agent
    if not agent:
        die("--agent is required: every claim must name a person.")
    if len(agent) > MAX_AGENT_LEN:
        die(f"--agent is too long ({len(agent)} > {MAX_AGENT_LEN}).")
    if a.hours <= 0:
        die("--hours must be greater than zero; use `release` to give a file back.")

    with locked() as data:
        held = find(data, path)
        stale = data["claims"].get(path)

        if held and held["agent"] != agent and not a.force:
            print(f"ownership: {path} is claimed.", file=sys.stderr)
            print(show(path, held), file=sys.stderr)
            print("       Do not edit it. Wait, ask them to `release`, or if "
                  "they are gone:\n"
                  "       ./tools/ownership.py claim "
                  f"{path} --agent {agent} --force", file=sys.stderr)
            return 1

        if held and held["agent"] == agent:
            print(f"ownership: {path} already claimed by you; refreshing.", file=sys.stderr)

        exp = now() + _dt.timedelta(hours=a.hours)
        data["claims"][path] = {"agent": agent, "since": iso(now()),
                                "expires": iso(exp), "pid": os.getpid()}
        if stale and stale["agent"] != agent:
            print(f"ownership: took over {path} from {stale['agent']} "
                  f"({stale['pid']}).", file=sys.stderr)
        save(data)

    print(f"ownership: claimed {path}  for {human(_dt.timedelta(hours=a.hours))} "
          f"(expires {iso(exp)})")
    return 0


def cmd_release(a) -> int:
    path = norm(a.path)
    with locked() as data:
        c = data["claims"].get(path)
        if not c:
            print(f"ownership: {path} was not claimed. Nothing to release.")
            return 0
        if c["agent"] != a.agent and not a.force:
            print(f"ownership: {path} is claimed by {c['agent']}, not you.",
                  file=sys.stderr)
            print(f"       If that agent is gone: --force", file=sys.stderr)
            return 1
        del data["claims"][path]
        save(data)
    who = " (not yours)" if c["agent"] != a.agent else ""
    print(f"ownership: released {path}{who}, was held by {c['agent']}.")
    return 0


def cmd_clear(a) -> int:
    if not a.force:
        print("ownership: clear removes every claim in the store, "
              "yours and everyone else's.", file=sys.stderr)
        print("       Are you sure? If so, run it again with --force.",
              file=sys.stderr)
        return 1
    with locked() as data:
        n = len(data["claims"])
        data["claims"] = {}
        save(data)
    print(f"ownership: cleared {n} claim(s).")
    return 0


def cmd_view(a) -> int:
    data = load()
    claims = data["claims"]
    live = {p: c for p, c in claims.items() if not expired(c)}
    dead = {p: c for p, c in claims.items() if expired(c)}

    if a.file:
        path = norm(a.file)
        c = live.get(path)
        if c:
            print(f"{path}")
            print(show(path, c))
            return 1 if a.exit_code else 0
        stale = dead.get(path)
        if stale:
            print(f"{path}\n  expired {human(now() - parse(stale['expires']))} ago, "
                  f"held by {stale['agent']} -- treated as free")
        else:
            print(f"{path}\n  free")
        return 0

    if not live and not dead:
        print("ownership: no claims. Every file in the tree is free.")
        return 0

    if live:
        print(f"ownership: {len(live)} active claim(s)")
        for p in sorted(live):
            print(f"  {p}")
            print(show(p, live[p]))
    if dead:
        print(f"ownership: {len(dead)} expired claim(s), treated as free")
        for p in sorted(dead):
            print(f"  {p}")
            print(f"  expired {human(now() - parse(dead[p]['expires']))} ago, "
                  f"held by {dead[p]['agent']}")
    return 0


def cmd_check(a) -> int:
    """Exit 0 if free/expired, 1 if held by someone else, 2 if held by you."""
    path = norm(a.path)
    data = load()
    c = find(data, path)
    if not c:
        return 0
    if c["agent"] == a.agent:
        return 2
    return 1


def cmd_prune(a) -> int:
    with locked() as data:
        dead = [p for p, c in data["claims"].items() if expired(c)]
        for p in dead:
            del data["claims"][p]
        if dead:
            save(data)
    if dead:
        print(f"ownership: pruned {len(dead)} expired claim(s): "
              + ", ".join(sorted(dead)))
    else:
        print("ownership: nothing to prune.")
    return 0


def cmd_import_legacy(a) -> int:
    """One-shot migration from the old ownership.txt format."""
    src = Path(a.path)
    if not src.exists():
        die(f"{src} not found.")
    rows = []
    for line in src.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.lower().startswith("none"):
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        path, agent, ts = parts[0], parts[1], parts[2]
        try:
            (REPO / path).resolve().relative_to(REPO)
        except ValueError:
            continue
        rows.append((path, agent, ts))
    if not rows:
        print("ownership: legacy file had no usable claims.")
        return 0
    with locked() as data:
        for path, agent, ts in rows:
            data["claims"][path] = {"agent": agent, "since": ts,
                                    "expires": iso(now() + _dt.timedelta(hours=DEFAULT_HOURS)),
                                    "pid": 0, "imported": True}
        save(data)
    print(f"ownership: imported {len(rows)} claim(s) with a {DEFAULT_HOURS}h TTL.")
    return 0


# ------------------------------------------------------------------ main ---

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="ownership.py",
        description="Claim, release and inspect file ownership for shared-tree work.")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("claim", help="claim a file")
    c.add_argument("path")
    c.add_argument("--agent", required=True, help="your name, so blockers can find you")
    c.add_argument("--hours", type=float, default=DEFAULT_HOURS,
                   help=f"claim lifetime (default {DEFAULT_HOURS})")
    c.add_argument("--force", action="store_true",
                   help="take the file from whoever holds it")
    c.set_defaults(fn=cmd_claim)

    c = sub.add_parser("release", help="give a file back")
    c.add_argument("path")
    c.add_argument("--agent", required=True)
    c.add_argument("--force", action="store_true")
    c.set_defaults(fn=cmd_release)

    c = sub.add_parser("view", help="list claims")
    c.add_argument("--file", help="show one file instead of all")
    c.add_argument("--exit-code", action="store_true",
                   help="exit 1 if --file is held by someone else (for scripts)")
    c.set_defaults(fn=cmd_view)

    c = sub.add_parser("check", help="exit 0 free, 1 held by other, 2 held by you")
    c.add_argument("path")
    c.add_argument("--agent", default="")
    c.set_defaults(fn=cmd_check)

    c = sub.add_parser("clear", help="remove all claims")
    c.add_argument("--force", action="store_true",
                   help="actually clear; without it this only warns")
    c.set_defaults(fn=cmd_clear)

    c = sub.add_parser("prune", help="delete expired claims")
    c.set_defaults(fn=cmd_prune)

    c = sub.add_parser("import-legacy", help="migrate the old ownership.txt")
    c.add_argument("path", nargs="?", default="ownership.txt")
    c.set_defaults(fn=cmd_import_legacy)

    a = p.parse_args(argv)
    try:
        return a.fn(a)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""cricket_score.py - live international cricket scores on the LED matrix.

Fetches the live score from ESPN's public cricket scoreboard (no account or API
key needed) and sends it to the ESP32 LED matrix. The matrix rotates through
the score, the overs, the chase or run rate and the two batters, and plays an
animation for every four, six, wicket, fifty, hundred and the result.

Talks to the board over the USB serial console by default (the same cable that
powers it). Use --http 192.168.4.1 only if this PC reaches the board's Wi-Fi
and the internet at the same time (for example Wi-Fi to the board, Ethernet to
the router).

Usage:
    python cricket_score.py                   # first live international match
    python cricket_score.py --team IND        # only follow India
    python cricket_score.py --list            # show today's matches and exit
    python cricket_score.py --match 1550711   # follow one match by its id
    python cricket_score.py --all             # include domestic and league matches
    python cricket_score.py --demo            # no internet: play a made-up over

Requires: pip install pyserial   (only for the serial transport)
Stop with Ctrl+C. The scoreboard is closed on exit and the eyes or text return.

Note: the USB serial port can be held by one program only, so this cannot run
at the same time as break_monitor.py on the same cable.
"""

import argparse
import datetime
import gzip
import json
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

from break_monitor import HttpLink, SerialLink, log, status

HEADER_URL = "https://site.web.api.espn.com/apis/v2/scoreboard/header?sport=cricket&lang=en&region=in"
SUMMARY_URL = "https://site.web.api.espn.com/apis/site/v2/sports/cricket/{league}/summary?event={event}"

MAX_PAGES = 8          # limits of the firmware (CRK_PAGES_MAX, CRK_PAGE_LEN, CRK_LINE_MAX)
MAX_PAGE_LEN = 39
MAX_PAYLOAD = 230
FONT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-+*.!: ")
RESULT_LINGER_S = 600  # keep the result on the matrix this long after the match ends
SUMMARY_REFRESH_S = 60 # refetch the detailed scorecard at least this often


# ── Fetching ─────────────────────────────────────────────────────────────────

def fetch_json(url):
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0",
        "Accept": "application/json",
        "Accept-Encoding": "gzip",
    })
    with urllib.request.urlopen(req, timeout=20) as resp:
        raw = resp.read()
        if resp.headers.get("Content-Encoding", "").lower() == "gzip":
            raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8", "replace"))


def list_matches(header):
    """Flatten the scoreboard into one dict per match."""
    out = []
    for sport in header.get("sports", []):
        for league in sport.get("leagues", []):
            for ev in league.get("events", []):
                cls = ev.get("class") or {}
                full = ev.get("fullStatus") or {}
                teams = []
                for c in sorted(ev.get("competitors", []), key=lambda c: c.get("order", 0)):
                    teams.append({
                        "id": str(c.get("id", "")),
                        "abbr": (c.get("abbreviation") or c.get("name") or "?").upper(),
                        "name": c.get("displayName") or "",
                        "score": c.get("score") or "",
                    })
                if len(teams) != 2:
                    continue
                out.append({
                    "id": str(ev.get("id", "")),
                    "league": str(league.get("id", "")),
                    "series": league.get("name", ""),
                    "name": ev.get("name", ""),
                    "state": ev.get("status", ""),            # pre / in / post
                    "summary": ev.get("summary", ""),         # Live, Stumps, Innings break, Result...
                    "long": full.get("longSummary") or full.get("summary") or "",
                    "batting_id": str(full.get("battingTeamId") or ""),
                    "intl_class": str(cls.get("internationalClassId") or "0"),
                    "card": cls.get("generalClassCard", ""),
                    "type": ev.get("eventType", ""),          # Test / ODI / T20
                    "date": ev.get("date", ""),
                    "teams": teams,
                })
    return out


def is_international(m):
    return m["intl_class"] not in ("", "0")


def starts_in_seconds(m):
    try:
        when = datetime.datetime.fromisoformat(m["date"].replace("Z", "+00:00"))
        return (when - datetime.datetime.now(datetime.timezone.utc)).total_seconds()
    except Exception:
        return None


def choose_match(matches, args, tracked_id, done_ids):
    """The match to show: the one already being followed, else a live one, else one about to start."""
    by_id = {m["id"]: m for m in matches}
    if args.match:
        return by_id.get(str(args.match))
    if tracked_id and tracked_id in by_id:
        return by_id[tracked_id]

    pool = [m for m in matches if (args.all or is_international(m)) and m["id"] not in done_ids]
    if args.team:
        want = args.team.upper()
        pool = [m for m in pool
                if any(t["abbr"] == want or want in t["name"].upper() for t in m["teams"])]
    # Men's Tests, ODIs and T20Is (classes 1 to 3) before the other international classes
    pool.sort(key=lambda m: 0 if m["intl_class"] in ("1", "2", "3") else 1)

    for m in pool:
        if m["state"] == "in":
            return m
    for m in pool:
        secs = starts_in_seconds(m)
        if m["state"] == "pre" and secs is not None and secs < 2 * 3600:
            return m
    return None


# ── Reading the score ────────────────────────────────────────────────────────

SCORE_RE = re.compile(r"(\d+)(?:/(\d+))?(d)?\s*(?:\(([\d.]+)(?:/(\d+))?\s*ov(?:,\s*target\s*(\d+))?\))?")


def parse_score(text):
    """'168 & 26/2 (8 ov, target 155)' -> the innings in progress (the last one)."""
    last = text.split("&")[-1].strip()
    mt = SCORE_RE.search(last)
    if not mt or not last:
        return None
    return {
        "runs": int(mt.group(1)),
        "wkts": int(mt.group(2)) if mt.group(2) else 10,
        "overs": mt.group(4) or "",
        "max_overs": int(mt.group(5)) if mt.group(5) else None,
        "target": int(mt.group(6)) if mt.group(6) else 0,
    }


def state_from_header(m):
    """Score of the batting side from the scoreboard alone. No boundary counts or batters."""
    bat = None
    for t in m["teams"]:
        if " ov" in t["score"].split("&")[-1]:
            bat = t
    if bat is None and m["batting_id"]:
        bat = next((t for t in m["teams"] if t["id"] == m["batting_id"]), None)
    if bat is None:
        return None
    sc = parse_score(bat["score"])
    if sc is None:
        return None
    innings = bat["score"].count("&") + 1
    return {
        "key": (m["id"], bat["abbr"], innings),
        "bat": bat["abbr"], "runs": sc["runs"], "wkts": sc["wkts"], "overs": sc["overs"],
        "max_overs": sc["max_overs"], "target": sc["target"],
        "fours": None, "sixes": None, "batters": [], "status": "",
    }


def state_from_summary(summary, m):
    """The same, from the detailed scorecard: adds fours, sixes and the two batters."""
    comp = summary["header"]["competitions"][0]
    current = None
    for c in comp.get("competitors", []):
        for ls in c.get("linescores", []):
            if ls.get("isCurrent") and ls.get("isBatting"):
                current = (c, ls)
    if current is None:
        return None
    c, ls = current
    team = c.get("team", {})
    abbr = (team.get("abbreviation") or "?").upper()
    period = ls.get("period")

    batters = []
    for side in summary.get("rosters", []):
        side_team = side.get("team", {})
        if str(side_team.get("id")) != str(c.get("id")) and (side_team.get("abbreviation") or "").upper() != abbr:
            continue
        for player in side.get("roster", []):
            for per in player.get("linescores", []):
                if per.get("period") != period:
                    continue
                for line in per.get("linescores", []):
                    stats = {s["name"]: s.get("value", 0)
                             for cat in line.get("statistics", {}).get("categories", [])
                             for s in cat.get("stats", [])}
                    if stats.get("batted") and not stats.get("outs"):
                        ath = player.get("athlete", {})
                        name = ath.get("lastName") or (ath.get("displayName") or "?").split()[-1]
                        batters.append((clean(name), int(stats.get("runs", 0)), int(stats.get("battingPosition", 0))))
    batters.sort(key=lambda b: b[2])

    head = state_from_header(m) or {}
    return {
        "key": (m["id"], abbr, period),
        "bat": abbr,
        "runs": int(ls.get("runs", 0)),
        "wkts": int(ls.get("wickets", 0)),
        "overs": "%.1f" % float(ls.get("overs", 0)),
        "max_overs": head.get("max_overs"),
        "target": int(ls.get("target") or 0) or head.get("target", 0),
        "fours": int(ls.get("fours", 0)),
        "sixes": int(ls.get("sixes", 0)),
        "batters": [(b[0], b[1]) for b in batters[-2:]],
        "status": (comp.get("status") or {}).get("summary", ""),
    }


# ── Turning the score into pages and animations ──────────────────────────────

def clean(text):
    """Upper-case ASCII limited to the characters the matrix font has."""
    text = unicodedata.normalize("NFKD", str(text)).encode("ascii", "ignore").decode("ascii").upper()
    text = text.replace("&", "+").replace("'", "")
    text = "".join(ch if ch in FONT_CHARS else " " for ch in text)
    return re.sub(r"\s+", " ", text).strip()


def strip_parens(text):
    return re.sub(r"\([^)]*\)", "", text).strip()


def shorten(text, m):
    """Use team abbreviations and drop bracketed asides so a sentence scrolls quickly."""
    text = strip_parens(text)
    for t in sorted(m["teams"], key=lambda t: -len(t["name"])):
        if t["name"]:
            text = re.sub(re.escape(t["name"]), t["abbr"], text, flags=re.IGNORECASE)
    return clean(text)


def max_overs_for(m, st):
    if st.get("max_overs"):
        return st["max_overs"]
    if is_international(m):
        return {"T20": 20, "ODI": 50}.get(m["type"])
    return None


def balls_bowled(overs):
    whole, _, part = overs.partition(".")
    return int(whole or 0) * 6 + int(part or 0)


def live_pages(m, st):
    """Pages for a match in progress. Returns (balls in this over, pages)."""
    pages = [f"{st['bat']} {st['runs']}" + ("" if st["wkts"] >= 10 else f"/{st['wkts']}")]
    if st["overs"]:
        pages.append(f"OV {st['overs']}")

    bowled = balls_bowled(st["overs"]) if st["overs"] else 0
    limit = max_overs_for(m, st)
    chasing = st["target"] > st["runs"]
    if chasing:
        pages.append(f"NEED {st['target'] - st['runs']}")
        if limit and limit * 6 > bowled:
            pages.append(f"{limit * 6 - bowled} BALLS")
    elif limit and bowled:
        pages.append("CRR %.2f" % (st["runs"] * 6.0 / bowled))

    for name, runs in st["batters"]:
        pages.append(f"{name} {runs}*")

    other = next((t for t in m["teams"] if t["abbr"] != st["bat"]), None)
    if other and other["score"]:
        pages.append(clean(f"{other['abbr']} {strip_parens(other['score'])}"))

    if m["summary"] and m["summary"].lower() != "live":
        pages.append(clean(m["summary"]))                    # STUMPS, INNINGS BREAK, RAIN...
    if st["status"] and not chasing:
        pages.append(shorten(st["status"], m))               # SL TRAIL BY 30 RUNS

    balls = (bowled % 6) if st["overs"] else "-"
    return balls, pages


def fit(pages):
    """Trim to what one firmware command can carry."""
    out, total = [], 2
    for page in pages:
        page = clean(page)[:MAX_PAGE_LEN].strip()
        if not page or page in out:
            continue
        if len(out) >= MAX_PAGES or total + len(page) + 1 > MAX_PAYLOAD:
            break
        out.append(page)
        total += len(page) + 1
    return out


def detect_events(prev, cur):
    """Animations to play for what happened between two polls of the same innings."""
    if prev is None or prev["key"] != cur["key"]:
        return []
    events = []
    exact = cur["fours"] is not None and prev["fours"] is not None
    if exact:
        if cur["fours"] > prev["fours"]:
            events.append(("4", ""))
        if cur["sixes"] > prev["sixes"]:
            events.append(("6", ""))
    else:
        # Scoreboard only: guess from the jump in the total
        gained = cur["runs"] - prev["runs"]
        if gained in (4, 5):
            events.append(("4", ""))
        elif gained in (6, 7):
            events.append(("6", ""))
    if cur["wkts"] > prev["wkts"]:
        events.append(("W", ""))
    before = dict(prev["batters"])
    for name, runs in cur["batters"]:
        was = before.get(name)
        if was is None:
            continue
        if was < 100 <= runs:
            events.append(("100", f"{name} {runs}*"))
        elif was < 50 <= runs:
            events.append(("50", f"{name} {runs}*"))
    return events[:3]


# ── The board ────────────────────────────────────────────────────────────────

class Board:
    """Sends the firmware's cricket commands over USB serial or HTTP."""

    def __init__(self, args):
        self.http = HttpLink(args.http) if args.http else None
        self.serial = None if args.http else SerialLink(args.port)

    def score(self, balls, pages):
        payload = f"{balls}|" + "|".join(pages)
        if self.http:
            return self.http._get("/cricket?d=" + urllib.parse.quote(payload))
        return self.serial.send("q " + payload)

    def event(self, code, text=""):
        if self.http:
            path = "/cricketEvent?e=" + code + ("&text=" + urllib.parse.quote(text) if text else "")
            reply = self.http._get(path)
            time.sleep(0.2)          # the board holds one web command at a time
            return reply
        return self.serial.send(f"v {code} {text}".strip())

    def stop(self):
        if self.http:
            return self.http._get("/cricketStop")
        return self.serial.send("Q")


# ── Modes ────────────────────────────────────────────────────────────────────

def print_matches(matches):
    if not matches:
        print("No matches on the scoreboard right now.")
        return
    print(f"{'id':>8}  {'state':5}  {'intl':4}  match")
    for m in matches:
        scores = "  ".join(f"{t['abbr']} {t['score']}".strip() for t in m["teams"])
        print(f"{m['id']:>8}  {m['state']:5}  {'yes' if is_international(m) else '':4}  "
              f"{m['name']} ({m['card']}, {m['summary']})  {scores}")


def run_demo(board):
    """A made-up finish that shows every page and every animation."""
    steps = [
        (None, 2, ["IND 187/4", "OV 18.2", "NEED 24", "10 BALLS", "KOHLI 46*", "PANDYA 31*"], 14),
        (("4", ""), 3, ["IND 191/4", "OV 18.3", "NEED 20", "9 BALLS", "KOHLI 50*", "PANDYA 31*"], 5),
        (("50", "KOHLI 50*"), 3, ["IND 191/4", "OV 18.3", "NEED 20", "9 BALLS", "KOHLI 50*", "PANDYA 31*"], 12),
        (("6", ""), 4, ["IND 197/4", "OV 18.4", "NEED 14", "8 BALLS", "KOHLI 56*", "PANDYA 31*"], 10),
        (("W", ""), 5, ["IND 197/5", "OV 18.5", "NEED 14", "7 BALLS", "KOHLI 56*", "JADEJA 0*"], 10),
        (None, 0, ["IND 199/5", "OV 19.0", "NEED 12", "6 BALLS", "KOHLI 57*", "JADEJA 1*"], 8),
        (("6", ""), 1, ["IND 205/5", "OV 19.1", "NEED 6", "5 BALLS", "KOHLI 63*", "JADEJA 1*"], 8),
        (("6", ""), 2, ["IND 211/5", "OV 19.2", "KOHLI 69*", "JADEJA 1*"], 6),
        (("WIN", "IND WON BY 5 WICKETS"), "-", ["IND V AUS", "IND WON BY 5 WICKETS"], 20),
    ]
    log("demo: a made-up finish, about 90 seconds")
    for event, balls, pages, hold in steps:
        board.score(balls, pages)                 # also brings the scoreboard up for the first step
        if event:
            board.event(*event)
            log(f"animation {event[0]} {event[1]}".rstrip())
        log("score  " + " | ".join(pages))
        time.sleep(hold)
    board.stop()
    log("demo finished")


def run_live(board, args):
    tracked_id = None      # match being followed
    done_ids = set()       # finished matches we have stopped showing
    prev = None            # last score state, for spotting fours, sixes and wickets
    detail = None          # last detailed scorecard state
    detail_sig = None
    detail_at = 0.0
    result_since = None
    showing = False
    last_line = None

    log("looking for a live international match" + (f" with {args.team.upper()}" if args.team else "")
        + " (use --list to see all matches)")

    while True:
        try:
            matches = list_matches(fetch_json(HEADER_URL))
        except Exception as exc:
            log(f"could not fetch the scoreboard: {exc}")
            time.sleep(max(args.poll, 15))
            continue

        m = choose_match(matches, args, tracked_id, done_ids)
        if m is None:
            if showing:
                board.stop()
                showing = False
            tracked_id, prev, detail, result_since = None, None, None, None
            status("no live international match right now, checking again in 60 s")
            time.sleep(60)
            continue

        if m["id"] != tracked_id:
            tracked_id, prev, detail, detail_sig, result_since = m["id"], None, None, None, None
            log(f"following {m['name']} - {m['series']} ({m['card']}), match id {m['id']}")

        events = []
        if m["state"] == "in":
            sig = tuple(t["score"] for t in m["teams"]) + (m["summary"],)
            if sig != detail_sig or time.time() - detail_at > SUMMARY_REFRESH_S:
                try:
                    detail = state_from_summary(fetch_json(SUMMARY_URL.format(league=m["league"], event=m["id"])), m)
                except Exception as exc:
                    log(f"scorecard unavailable, using the scoreboard only: {exc}")
                    detail = None
                detail_sig, detail_at = sig, time.time()
            st = detail or state_from_header(m)
            if st is None:
                balls, pages = "-", [f"{m['teams'][0]['abbr']} V {m['teams'][1]['abbr']}",
                                     shorten(m["long"] or m["summary"], m)]
            else:
                events = detect_events(prev, st)
                prev = st
                balls, pages = live_pages(m, st)
        elif m["state"] == "pre":
            pages = [f"{m['teams'][0]['abbr']} V {m['teams'][1]['abbr']}"]
            secs = starts_in_seconds(m)
            if secs is not None:
                start = datetime.datetime.now() + datetime.timedelta(seconds=secs)
                pages.append(start.strftime("AT %H:%M"))
            balls = "-"
        else:  # finished
            result = shorten(m["long"] or m["summary"], m)
            if result_since is None:
                result_since = time.time()
                if showing:
                    events = [("WIN", result)]
                    log(f"result: {result}")
            if time.time() - result_since > RESULT_LINGER_S or (not showing and not args.match):
                done_ids.add(m["id"])
                tracked_id = None
                if args.match:
                    board.stop()
                    log("the match has finished")
                    return
                time.sleep(1)
                continue
            pages = [f"{m['teams'][0]['abbr']} V {m['teams'][1]['abbr']}", result]
            for t in m["teams"]:
                if t["score"]:
                    pages.append(f"{t['abbr']} {strip_parens(t['score'])}")
            balls = "-"

        pages = fit(pages)
        if not pages:
            pages = [clean(f"{m['teams'][0]['abbr']} V {m['teams'][1]['abbr']}")]
        if not showing:
            board.score(balls, pages)        # animations only play once the scoreboard is up
            showing = True
        for code, text in events:
            board.event(code, clean(text))
            log(f"animation {code} {text}".rstrip())
        if board.score(balls, pages) is None:
            log("the board did not answer; will retry")
            showing = False

        line = " | ".join(pages)
        if line != last_line:
            log(line)
            last_line = line
        status(f"{line[:60]}   (updated {time.strftime('%H:%M:%S')})")
        time.sleep(args.poll)


def main():
    ap = argparse.ArgumentParser(description="Show live international cricket scores on the LED matrix.")
    ap.add_argument("--team", default=None, help="only follow matches of this team, e.g. IND or Australia")
    ap.add_argument("--match", default=None, help="follow this match id (see --list)")
    ap.add_argument("--all", action="store_true", help="also follow domestic and league matches")
    ap.add_argument("--list", action="store_true", help="print the matches on the scoreboard and exit")
    ap.add_argument("--demo", action="store_true", help="play a made-up finish with every animation, no internet needed")
    ap.add_argument("--poll", type=float, default=15, help="seconds between score checks (default 15)")
    ap.add_argument("--port", default=None, help="serial port, e.g. COM7 (default: auto-detect the ESP32-C3)")
    ap.add_argument("--http", default=None, metavar="HOST", help="use the web endpoints at HOST instead of serial")
    args = ap.parse_args()

    if args.list:
        print_matches(list_matches(fetch_json(HEADER_URL)))
        return

    board = Board(args)
    try:
        if args.demo:
            run_demo(board)
        else:
            run_live(board, args)
    except KeyboardInterrupt:
        print()
        board.stop()
        log("stopped, scoreboard closed")


if __name__ == "__main__":
    main()

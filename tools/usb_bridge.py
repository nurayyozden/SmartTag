#!/usr/bin/env python3
"""USB bridge between Supabase and the NFC station (no Wi-Fi on the ESP32).

The website saves garments to Supabase with tag_status = 'pending'. This
script runs on the laptop the ESP32 is plugged into and:

  1. checks Supabase every few seconds for the oldest pending garment,
  2. sends it to the ESP32 over USB:      JOB <clothing_id> <record>
  3. waits for the ESP32 to write a tag:  @WROTE <clothing_id> <uid>
  4. sets tag_status = 'written' and tag_uid, so the website shows
     "Tag written".

It also shows everything the ESP32 prints, so it replaces the serial
monitor (close the PlatformIO monitor first: only one program can use the
port). Type  w  or  s  + Enter to switch the station between WRITE and SORT.

Run (PlatformIO's Python already has pyserial):
    ~/.platformio/penv/bin/python tools/usb_bridge.py
or, after `pip3 install pyserial`:
    python3 tools/usb_bridge.py [--port /dev/cu.usbserial-0001]
"""

import argparse
import glob
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Run with ~/.platformio/penv/bin/python, "
             "or install it: pip3 install pyserial")

ROOT = Path(__file__).resolve().parent.parent
POLL_SECONDS = 3
BAUD = 115200

UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
TAG_UID_RE = re.compile(r"(?:[0-9A-F]{2}:){3,9}[0-9A-F]{2}")


# ---------------------------------------------------------------
# Supabase
# ---------------------------------------------------------------

def load_config():
    """Reads SUPABASE_URL and SUPABASE_KEY from web/config.js."""
    text = (ROOT / "web" / "config.js").read_text()
    url = re.search(r'SUPABASE_URL\s*=\s*"([^"]+)"', text)
    key = re.search(r'SUPABASE_KEY\s*=\s*"([^"]+)"', text)
    if not url or not key or "YOUR-" in url.group(1) or "YOUR-" in key.group(1):
        sys.exit("Fill in SUPABASE_URL and SUPABASE_KEY in web/config.js first.")
    return url.group(1).rstrip("/"), key.group(1)


class Supabase:
    def __init__(self, url, key):
        self.base = url + "/rest/v1/"
        self.key = key

    def _request(self, method, path, body=None):
        req = urllib.request.Request(
            self.base + path,
            method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={
                "apikey": self.key,
                "Content-Type": "application/json",
                "Prefer": "return=representation",
            },
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read() or "null")

    def next_job(self):
        """Returns (clothing_id, record) for the oldest pending garment, or None."""
        rows = self._request(
            "GET",
            "clothing?select=clothing_id,category_id,colour,fibre_unknown,"
            "garment_fibre(percentage,fibre(code))"
            "&tag_status=eq.pending&order=created_at.asc&limit=1"
            "&garment_fibre.order=percentage.desc",
        )
        if not rows:
            return None
        row = rows[0]
        return row["clothing_id"], build_record(row)

    def mark_written(self, clothing_id, uid):
        """True if the row changed. With row-level security a blocked update
        returns [] instead of an error, so the result is checked.
        `uid` may be None if it arrived damaged; tag_uid then stays empty."""
        body = {"tag_status": "written"}
        if uid:
            body["tag_uid"] = uid
        rows = self._request(
            "PATCH",
            f"clothing?clothing_id=eq.{clothing_id}&tag_status=eq.pending",
            body,
        )
        return bool(rows)


def build_record(row):
    """Same text as buildTagRecord() in web/analysis.js, e.g.
    {"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}"""
    rec = {"id": str(row["clothing_id"])[:8], "cat": row["category_id"], "col": row["colour"]}
    # Supabase returns percentages like 99.0; write 99, as the website does.
    pct = lambda v: int(v) if float(v).is_integer() else v
    fibres = [
        [gf["fibre"]["code"], pct(gf["percentage"])]
        for gf in row.get("garment_fibre") or []
        if gf.get("fibre")
    ]
    if not row.get("fibre_unknown") and fibres:
        rec["f"] = fibres
    return json.dumps(rec, separators=(",", ":"))


# ---------------------------------------------------------------
# Serial
# ---------------------------------------------------------------

def looks_like_text(line):
    """False for the ESP32's own boot messages: lines with unreadable
    characters (sent at 74880 baud) and the chip's start-up report. The
    firmware itself only prints plain ASCII."""
    if any(not c.isprintable() or c == "�" for c in line):
        return False
    return not line.startswith(BOOT_PREFIXES)


BOOT_PREFIXES = ("rst:", "ets ", "configsip:", "clk_drv:", "mode:", "load:", "entry ")


def is_subsequence(word, of):
    it = iter(of)
    return all(c in it for c in word)


def parse_wrote(line):
    """Recognises the ESP32's "@WROTE <clothing_id> <tag uid>" line, even
    when characters were lost on the cable (it happens while the NFC radio
    is busy, e.g. "@WOTE c23c1bb1-1d0e-..." or a shortened id).

    Returns (clothing_id or None, tag_uid or None) if the line looks like a
    write confirmation, otherwise None. A None part was too damaged to use."""
    parts = line.split()
    if not parts or len(parts[0]) < 4 or not is_subsequence(parts[0], "@WROTE"):
        return None
    clothing_id = parts[1] if len(parts) > 1 and UUID_RE.fullmatch(parts[1]) else None
    uid = parts[2] if len(parts) > 2 and TAG_UID_RE.fullmatch(parts[2]) else None
    return clothing_id, uid


def find_port():
    patterns = ["/dev/cu.usbserial*", "/dev/cu.SLAB*", "/dev/cu.wchusbserial*",
                "/dev/ttyUSB*", "/dev/ttyACM*"]
    ports = sorted(p for pat in patterns for p in glob.glob(pat))
    if not ports:
        sys.exit("No ESP32 found. Plug it in, or pass --port.")
    if len(ports) > 1:
        print(f"Several ports found, using {ports[0]} (others: {', '.join(ports[1:])})")
    return ports[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", help="serial port, e.g. /dev/cu.usbserial-0001")
    args = parser.parse_args()

    db = Supabase(*load_config())
    port = args.port or find_port()
    try:
        esp = serial.Serial(port, BAUD, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"Couldn't open {port}: {e}\n"
                 "Is the PlatformIO serial monitor (or an upload) still using it?")
    # Restart the ESP32 (same as esptool's hard reset: EN pulsed low via RTS,
    # with DTR released so it boots the firmware, not the bootloader).
    esp.dtr = False
    esp.rts = True
    time.sleep(0.1)
    esp.rts = False
    print(f"Connected to {port}. Restarting the ESP32; it takes a few seconds "
          "to be ready.\n")
    started = time.time()
    warned = False

    lock = threading.Lock()

    def send(line):
        with lock:
            esp.write((line + "\n").encode())

    # Forward typed commands (w / s) to the ESP32.
    def keyboard():
        for line in sys.stdin:
            if line.strip():
                send(line.strip())
    threading.Thread(target=keyboard, daemon=True).start()

    sent = None          # job last sent to the ESP32: (id, record), "CLEAR" or None
    ready = False        # ESP32 has printed @READY since the port opened
    to_mark = []         # (clothing_id, uid) written to tags, not yet saved
    confirmed = set()    # clothing_ids already queued: the ESP32 confirms 3 times
    next_poll = 0.0
    buf = b""

    try:
        while True:
            # ---- Lines from the ESP32
            buf += esp.read(esp.in_waiting or 1)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").rstrip("\r")
                if line == "@READY":
                    ready, sent, next_poll = True, None, 0
                elif (wrote := parse_wrote(line)) is not None:
                    clothing_id, uid = wrote
                    job_id = sent[0] if isinstance(sent, tuple) else None
                    if clothing_id is None and job_id:
                        # The ESP32 only ever writes the garment it was sent.
                        clothing_id = job_id
                    if clothing_id is None:
                        print(f"[bridge] WARNING: got a damaged write confirmation and "
                              f"no garment was outstanding, ignoring it: {line!r}")
                    elif clothing_id not in confirmed:
                        confirmed.add(clothing_id)
                        if uid is None:
                            print(f"[bridge] the tag UID arrived damaged ({line!r}); "
                                  "saving the garment as written without it")
                        to_mark.append((clothing_id, uid))
                        sent, next_poll = None, 0
                elif looks_like_text(line):
                    print(line)
                # else: the ESP32's start-up messages, sent at 74880 baud,
                # which show up as garbage at 115200. Ignore them.

            if time.time() < next_poll:
                continue
            next_poll = time.time() + POLL_SECONDS

            try:
                # ---- Save written tags first, so they aren't sent again
                while to_mark:
                    clothing_id, uid = to_mark[0]
                    try:
                        changed = db.mark_written(clothing_id, uid)
                    except urllib.error.HTTPError as e:
                        if not 400 <= e.code < 500:
                            raise  # server trouble: keep it and retry
                        # Supabase rejected the request itself; retrying
                        # won't help and would block every later garment.
                        print(f"[bridge] WARNING: Supabase refused to mark {clothing_id[:8]} "
                              f"as written ({e.code}: {e.read().decode(errors='replace')}). "
                              "Skipping it; it stays pending and will come up again.")
                        confirmed.discard(clothing_id)
                        to_mark.pop(0)
                        continue
                    if changed:
                        print(f"[bridge] Supabase updated: {clothing_id[:8]} -> tag {uid}. "
                              "The website now shows \"Tag written\".")
                    else:
                        print(f"[bridge] WARNING: {clothing_id[:8]} wasn't updated. Either it is "
                              "no longer pending, or the key has no UPDATE policy on clothing "
                              "(see src/nfc_station/README.md).")
                    to_mark.pop(0)

                # ---- Send the current job
                # Normally sent after @READY. If that never arrives (board
                # didn't restart), send anyway after a few seconds; the
                # firmware ignores repeats.
                if not ready:
                    if time.time() - started < 8:
                        continue
                    if not warned:
                        warned = True
                        print("[bridge] No @READY from the ESP32. If nothing else appears, "
                              "upload the nfc_station firmware (the old nfc_reader code "
                              "doesn't understand the bridge) or press the board's EN button.")
                job = db.next_job()
                want = job if job else "CLEAR"
                if want != sent:
                    send(f"JOB {job[0]} {job[1]}" if job else "CLEAR")
                    print(f"[bridge] sent garment {job[0][:8]} to the ESP32" if job
                          else "[bridge] no garments waiting for a tag")
                    sent = want
            except (urllib.error.URLError, OSError, ValueError) as e:
                if isinstance(e, serial.SerialException):
                    raise
                print(f"[bridge] Supabase error, retrying: {e}")
                if isinstance(e, urllib.error.HTTPError):
                    print(f"[bridge]   details: {e.read().decode(errors='replace')[:300]}")
    except serial.SerialException:
        sys.exit("\nESP32 disconnected.")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        esp.close()


if __name__ == "__main__":
    main()

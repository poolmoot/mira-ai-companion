"""Companion hub: the one program that owns the Atech board.

Everything else talks to the board through this hub's HTTP API:
  - the ElevenLabs agent (webhook tools under /tools/*)
  - your friend's dashboard (REST under /api/*, live updates on /ws)

Run:
    pip install fastapi "uvicorn[standard]" atech
    export MED_TIMES="08:00,20:00"         # when the medicine light turns red
    python3 hub.py                          # http://localhost:8000/docs
    python3 hub.py --fake                   # no board needed (dashboard dev)
"""

import argparse
import asyncio
import datetime as dt
import json
import os
import sqlite3
import threading
import time
from pathlib import Path

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

DB_FILE = Path(__file__).with_name("companion.db")
MED_TIMES = [t.strip() for t in os.getenv("MED_TIMES", "08:00,20:00").split(",") if t.strip()]
MED_NAME = os.getenv("MED_NAME", "2 Keytruda pills")   # shown on the screen when due


# --- Storage ----------------------------------------------------------------

db_lock = threading.Lock()
db = sqlite3.connect(DB_FILE, check_same_thread=False)
db.row_factory = sqlite3.Row
db.executescript("""
CREATE TABLE IF NOT EXISTS med_log (id INTEGER PRIMARY KEY, due_at TEXT, taken_at TEXT, source TEXT);
CREATE TABLE IF NOT EXISTS mood_log (id INTEGER PRIMARY KEY, at TEXT, mood TEXT, score INTEGER, note TEXT);
CREATE TABLE IF NOT EXISTS readings (id INTEGER PRIMARY KEY, at TEXT, temperature REAL, humidity REAL);
CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, at TEXT, recipient TEXT, message TEXT);
""")


def now():
    return dt.datetime.now().isoformat(timespec="seconds")


def q(sql, args=()):
    with db_lock:
        cur = db.execute(sql, args)
        db.commit()
        return [dict(r) for r in cur.fetchall()]


# --- Live state + push to dashboards ----------------------------------------

state = {
    "board_connected": False,
    "med_due": False,
    "med_due_since": None,
    "next_med_time": None,
    "temperature": None,
    "humidity": None,
    "screen_text": "Hi!",
}
sockets: set[WebSocket] = set()
loop: asyncio.AbstractEventLoop | None = None


def publish(event, data=None):
    """Send {event, data, state} to every connected dashboard."""
    msg = json.dumps({"event": event, "data": data, "state": state, "at": now()})
    if loop:
        for ws in list(sockets):
            asyncio.run_coroutine_threadsafe(ws.send_text(msg), loop)


# --- Board ------------------------------------------------------------------

class FakeBoard:
    def call(self, key, value="", timeout=2.0):
        print(f"[fake board] {key} {value!r}")


board = None
board_lock = threading.Lock()


def act(key, value=""):
    if board is None:
        return
    with board_lock:
        try:
            board.call(key, value, timeout=10.0)   # the voice stream can delay the ack
        except Exception as e:
            print(f"board action {key} failed: {e}")


def board_reader():
    for ev in board.events(keys=["med_taken", "temp_temperature", "temp_humidity", "btn"]):
        if ev.key == "med_taken":
            record_intake("button")
        elif ev.key == "temp_temperature":
            state["temperature"] = round(float(ev.value), 1)
        elif ev.key == "temp_humidity":
            state["humidity"] = round(float(ev.value), 1)
            q("INSERT INTO readings (at, temperature, humidity) VALUES (?,?,?)",
              (now(), state["temperature"], state["humidity"]))
            publish("reading", {"temperature": state["temperature"], "humidity": state["humidity"]})
        elif ev.key == "btn" and ev.value == 1:
            publish("button")


# --- Medicine ---------------------------------------------------------------

def set_med_due(on: bool):
    state["med_due"] = on
    state["med_due_since"] = now() if on else None
    act("med_due", f"Take {MED_NAME}" if on else 0)
    publish("med_due" if on else "med_cleared")


def record_intake(source):
    """Called when the button is pressed while due, or when the agent/dashboard logs it."""
    if source != "button" and state["med_due"]:
        act("med_due", 0)       # the board turns the red light off on its own for button presses
    q("INSERT INTO med_log (due_at, taken_at, source) VALUES (?,?,?)",
      (state["med_due_since"], now(), source))
    state["med_due"] = False
    state["med_due_since"] = None
    publish("med_taken", {"source": source})


def next_med_time():
    t = dt.datetime.now().strftime("%H:%M")
    later = sorted(m for m in MED_TIMES if m > t)
    return later[0] if later else (sorted(MED_TIMES)[0] if MED_TIMES else None)


def med_scheduler():
    fired = set()
    while True:
        today = dt.date.today().isoformat()
        hhmm = dt.datetime.now().strftime("%H:%M")
        state["next_med_time"] = next_med_time()
        if hhmm in MED_TIMES and (today, hhmm) not in fired:
            fired.add((today, hhmm))
            if not state["med_due"]:
                set_med_due(True)
        time.sleep(15)


# --- API --------------------------------------------------------------------

app = FastAPI(title="Companion hub")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])


class Message(BaseModel):
    recipient: str
    message: str


class Text(BaseModel):
    text: str


class Color(BaseModel):
    r: int
    g: int
    b: int


class Mood(BaseModel):
    mood: str                 # e.g. "happy", "lonely", "tired", "in pain"
    score: int | None = None  # 1 (very bad) .. 5 (very good)
    note: str | None = None



class Schedule(BaseModel):
    times: list[str]          # ["08:00", "20:00"]


# Dashboard endpoints

@app.get("/api/state")
def get_state():
    return state | {"med_times": MED_TIMES, "med_name": MED_NAME}


@app.get("/api/meds")
def get_meds(limit: int = 50):
    return q("SELECT * FROM med_log ORDER BY id DESC LIMIT ?", (limit,))


@app.get("/api/messages")
def get_messages(limit: int = 50):
    return q("SELECT * FROM messages ORDER BY id DESC LIMIT ?", (limit,))


@app.get("/api/moods")
def get_moods(limit: int = 50):
    return q("SELECT * FROM mood_log ORDER BY id DESC LIMIT ?", (limit,))


@app.get("/api/readings")
def get_readings(limit: int = 200):
    return q("SELECT * FROM readings ORDER BY id DESC LIMIT ?", (limit,))


@app.put("/api/schedule")
def put_schedule(s: Schedule):
    MED_TIMES[:] = sorted(s.times)
    state["next_med_time"] = next_med_time()
    publish("schedule", MED_TIMES)
    return {"med_times": MED_TIMES}


@app.post("/api/med/due")
def api_med_due():
    set_med_due(True)
    return state


@app.post("/api/med/taken")
def api_med_taken():
    record_intake("dashboard")
    return state


@app.post("/api/screen")
def api_screen(t: Text):
    state["screen_text"] = t.text
    act("show", t.text)
    publish("screen", t.text)
    return state


@app.post("/api/lights")
def api_lights(c: Color):
    act("lights", f"{c.r},{c.g},{c.b}")
    return {"ok": True}


@app.websocket("/ws")
async def ws(websocket: WebSocket):
    await websocket.accept()
    sockets.add(websocket)
    await websocket.send_text(json.dumps({"event": "hello", "state": state, "at": now()}))
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        sockets.discard(websocket)


# ElevenLabs agent webhook tools (each returns {"result": "<sentence for the agent>"})

@app.api_route("/tools/medication_status", methods=["GET", "POST"])
def tool_med_status():
    last = q("SELECT taken_at FROM med_log ORDER BY id DESC LIMIT 1")
    return {"result": f"Medicine: {MED_NAME}. Due now: {'yes' if state['med_due'] else 'no'}. "
                      f"Last taken: {last[0]['taken_at'] if last else 'never'}. "
                      f"Next time: {state['next_med_time']}."}


@app.api_route("/tools/log_medication", methods=["GET", "POST"])
def tool_log_med():
    record_intake("voice")
    return {"result": "Logged. The light is off."}


@app.post("/tools/log_mood")
def tool_log_mood(m: Mood):
    q("INSERT INTO mood_log (at, mood, score, note) VALUES (?,?,?,?)", (now(), m.mood, m.score, m.note))
    publish("mood", m.model_dump())
    return {"result": "Mood saved."}


@app.api_route("/tools/room", methods=["GET", "POST"])
def tool_room():
    return {"result": f"Room temperature {state['temperature']} °C, humidity {state['humidity']} %."}


@app.post("/tools/send_message")
def tool_send_message(m: Message):
    # Demo only: logs the text and shows it on the device; nothing is actually sent.
    q("INSERT INTO messages (at, recipient, message) VALUES (?,?,?)", (now(), m.recipient, m.message))
    publish("message_sent", m.model_dump())
    state["screen_text"] = f"Message sent to {m.recipient}"
    threading.Thread(target=act, args=("show", state["screen_text"]), daemon=True).start()
    return {"result": f"Message sent to {m.recipient}."}


@app.post("/tools/show_on_screen")
def tool_show(t: Text):
    return {"result": "Shown." if api_screen(t) else "Failed."}



@app.on_event("startup")
async def startup():
    global loop
    loop = asyncio.get_running_loop()


def main():
    global board
    ap = argparse.ArgumentParser()
    ap.add_argument("--fake", action="store_true", help="run without a board")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    if args.fake:
        board = FakeBoard()
    else:
        from atech import Board
        board = Board.connect()
        threading.Thread(target=board_reader, daemon=True).start()
    state["board_connected"] = not args.fake
    threading.Thread(target=med_scheduler, daemon=True).start()
    uvicorn.run(app, host="0.0.0.0", port=args.port)


if __name__ == "__main__":
    main()

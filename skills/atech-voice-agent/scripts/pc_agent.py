#!/usr/bin/env python3
"""Run an ElevenLabs voice agent on this computer, optionally with an Atech board.

The computer's microphone listens. The agent's voice plays on the computer's
speakers, or on the Atech board's speaker over USB (--speaker board). The board
can also show a status light and give a push-to-talk button.

  pip install elevenlabs sounddevice numpy atech

  python3 pc_agent.py --agent-id agent_xxx                       # computer only
  python3 pc_agent.py --agent-id agent_xxx --speaker board --status-light
  python3 pc_agent.py --agent-id agent_xxx --activation button   # hold the board's button to talk

The agent must use these audio formats (the skill's setup sets them):
  user input audio  pcm_16000      agent output audio  pcm_16000

Private agent? Set ELEVENLABS_API_KEY. Stop with Ctrl-C.
"""

import argparse
import base64
import os
import queue
import signal
import threading
import time

import numpy as np

# python.org builds of Python on macOS ship without trusted certificates, so the
# secure connection to ElevenLabs fails ("CERTIFICATE_VERIFY_FAILED"). Use certifi's.
try:
    import certifi
    os.environ.setdefault("SSL_CERT_FILE", certifi.where())
except ImportError:
    pass

import sounddevice as sd
from elevenlabs.client import ElevenLabs
from elevenlabs.conversational_ai.conversation import AudioInterface, ClientTools, Conversation

RATE = 16000
MIC_BLOCK = 4000          # 250 ms per chunk, what ElevenLabs recommends
BOARD_CHUNK = 1024        # samples per spk_play_pcm action (~2.7k base64 chars, under the 3k limit)
ECHO_TAIL = 0.5           # keep the mic muted this long after the agent stops (no echo cancellation)


class Board:
    """The Atech board over USB: speaker, status light and button, all optional."""

    def __init__(self, port=None):
        from atech import Board as AtechBoard
        self.board = AtechBoard.connect(port=port) if port else AtechBoard.connect()
        self.lock = threading.Lock()

    def call(self, key, value=""):
        with self.lock:
            try:
                self.board.call(key, value, timeout=5.0)
            except Exception as e:
                print(f"[board] {key} failed: {e}")

    def events(self, keys):
        return self.board.events(keys=keys)


class Audio(AudioInterface):
    """Computer mic in; computer speakers or the board's speaker out (half duplex)."""

    def __init__(self, board=None, speaker="pc", push_to_talk=False):
        self.board = board
        self.speaker = speaker
        self.push_to_talk = push_to_talk
        self.talk_held = False
        self.out = queue.Queue()
        self.mute_until = 0.0
        self.play_end = 0.0
        self.running = False

    def start(self, input_callback):
        self.running = True

        def on_mic(indata, frames, t, status):
            muted = time.time() < self.mute_until or (self.push_to_talk and not self.talk_held)
            # Keep streaming silence while muted: the agent's turn detection needs a
            # continuous stream, and without echo cancellation it would otherwise
            # hear its own voice from the speaker and interrupt itself.
            input_callback(bytes(len(indata) * 2) if muted else indata.tobytes())

        self.mic = sd.InputStream(samplerate=RATE, channels=1, dtype="int16",
                                  blocksize=MIC_BLOCK, callback=on_mic)
        self.mic.start()
        if self.speaker == "pc":
            self.spk = sd.OutputStream(samplerate=RATE, channels=1, dtype="int16")
            self.spk.start()
        threading.Thread(target=self._player, daemon=True).start()

    def stop(self):
        self.running = False
        self.mic.stop()
        self.mic.close()
        if self.speaker == "pc":
            self.spk.stop()
            self.spk.close()

    def output(self, audio):
        step = BOARD_CHUNK * 2
        for i in range(0, len(audio), step):
            self.out.put(audio[i:i + step])

    def interrupt(self):
        while not self.out.empty():
            try:
                self.out.get_nowait()
            except queue.Empty:
                break

    def _player(self):
        while self.running:
            try:
                chunk = self.out.get(timeout=0.1)
            except queue.Empty:
                continue
            seconds = len(chunk) / 2 / RATE
            self.play_end = max(self.play_end, time.time()) + seconds
            self.mute_until = self.play_end + ECHO_TAIL
            if self.speaker == "pc":
                self.spk.write(np.frombuffer(chunk, dtype=np.int16))   # blocks: paces playback
            else:
                # spk_play_pcm returns once the chunk is queued to I2S, which paces us too
                self.board.call("spk_play_pcm", base64.b64encode(chunk).decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agent-id", required=True)
    ap.add_argument("--speaker", choices=["pc", "board"], default="pc")
    ap.add_argument("--activation", choices=["name", "always", "button"], default="name",
                    help="button = hold the board's button (instance btn) to talk")
    ap.add_argument("--status-light", action="store_true", help="board shows a circling light while connected")
    ap.add_argument("--volume", type=float, default=0.6, help="board speaker volume 0.0-1.0")
    ap.add_argument("--port", help="board serial port (default: first board found)")
    args = ap.parse_args()

    needs_board = args.speaker == "board" or args.status_light or args.activation == "button"
    board = Board(args.port) if needs_board else None
    if board and args.speaker == "board":
        board.call("spk_set_volume", args.volume)

    audio = Audio(board, args.speaker, push_to_talk=args.activation == "button")
    if args.activation == "button":
        def watch_button():
            for ev in board.events(["btn"]):
                audio.talk_held = ev.value == 1
        threading.Thread(target=watch_button, daemon=True).start()
        print("Hold the button and speak. Let go to hear the answer.")

    # Client tools run here on the computer: no public URL needed. Add your own:
    #   tools.register("tool_name", lambda params: "result text for the agent")
    # and create a *client* tool with the same name on the agent.
    tools = ClientTools()

    api_key = os.getenv("ELEVENLABS_API_KEY")
    stopping = threading.Event()

    def status(on):
        if board and args.status_light:
            board.call("agent_status", 1 if on else 0)

    # The SDK runs the connection in a thread; show its errors as one line, not a traceback.
    threading.excepthook = lambda a: print(f"Connection error: {a.exc_value}")

    # Keep one session open, and reconnect whenever ElevenLabs ends it.
    while not stopping.is_set():
        started = time.time()
        convo = Conversation(
            ElevenLabs(api_key=api_key),
            args.agent_id,
            requires_auth=bool(api_key),
            audio_interface=audio,
            client_tools=tools,
            callback_agent_response=lambda text: print(f"Agent: {text}"),
            callback_user_transcript=lambda text: print(f"You:   {text}"),
        )

        def stop(*_):
            stopping.set()
            convo.end_session()

        signal.signal(signal.SIGINT, stop)
        try:
            convo.start_session()
        except Exception as e:
            print(f"Could not connect: {e}. Retrying in 3 s.")
            time.sleep(3)
            continue
        status(True)
        print("Connected. Talk to your agent. Ctrl-C to stop.")
        convo.wait_for_session_end()
        status(False)
        if not stopping.is_set():
            # A session that ends within seconds never really connected: wait longer.
            wait = 5 if time.time() - started < 5 else 1
            print(f"Session ended, reconnecting in {wait} s...")
            time.sleep(wait)

if __name__ == "__main__":
    main()

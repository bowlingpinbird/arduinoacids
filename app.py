import json
import queue
import re
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial  # Requires: pip install pyserial
import serial.tools.list_ports

BAUD_RATE = 9600
BASE_DIR = Path(__file__).parent.resolve()

# Live page state. The serial thread writes it; HTTP handlers read it.
state_lock = threading.Lock()
LIVE = {
    "sequence": "",
    "status": "waiting",
    "message": "Waiting for Arduino...",
    "serial_line": "",
    "lines": [],
    "connected": False,
    "pdb": "",
    "log": "",
    "error": "",
}
cmd_queue = queue.Queue()
connected = threading.Event()
fold_seen = threading.Event()
fold_lock = threading.Lock()

SEQ_RE = re.compile(r"SEQ:\s*([A-Za-z]+)")
FINAL_RE = re.compile(r"^FINAL:\s*([A-Za-z]*)\s*$")


def update_state(**kwargs):
    with state_lock:
        LIVE.update(kwargs)


def snapshot():
    with state_lock:
        data = dict(LIVE)
        data["lines"] = list(LIVE["lines"])
        return data


def push_line(line):
    with state_lock:
        lines = LIVE["lines"]
        lines.append(line)
        del lines[:-12]
        LIVE["serial_line"] = line


def queue_command(cmd):
    """Send one firmware command. Returns False when the board is not connected."""
    if not connected.is_set():
        update_state(error="Arduino is not connected. Close the Serial Monitor.")
        return False
    cmd_queue.put(cmd if cmd.endswith("\n") else cmd + "\n")
    update_state(message=f"Sent to Arduino: {cmd.strip()}", error="")
    return True


def handle_arduino_line(line):
    """Pull the growing sequence out of the sketch's serial lines."""
    push_line(line)
    if line.startswith("Resistor sequencer ready") or line.startswith("RESET:"):
        update_state(
            sequence="",
            status="waiting",
            message="Add a resistor and close the topper.",
            error="",
        )
        return

    if line.startswith("Loop open"):
        update_state(status="open", message="Loop open. Add a resistor and close the topper.", error="")
        return

    if line.startswith("Loop closed"):
        update_state(status="closed", message=line, error="")
        return

    if line.startswith("Unrecognized"):
        update_state(status="building", message=line, error="")
        return

    final = FINAL_RE.match(line)
    if final:
        seq = final.group(1).upper()
        update_state(
            sequence=seq,
            status="stopping",
            message=f"Arduino stopped. Sequence: {seq or '(empty)'}",
        )
        fold_seen.set()
        return

    found = SEQ_RE.search(line)
    if found and (line.startswith("Added ") or line.startswith("SEQ:")):
        seq = found.group(1).upper()
        update_state(
            sequence=seq,
            status="building",
            message=f"Sequence: {seq}. Add the next resistor, or press Fold.",
            error="",
        )
        return

    update_state(message=line, error="")


def run_pipeline(sequence):
    """Runs predict.py and stores the PDB for the page."""
    sequence = sequence.strip().upper()
    if not sequence:
        raise ValueError("Empty sequence received.")

    print(f"Running pipeline for sequence: {sequence}")
    cmd1 = [sys.executable, str(BASE_DIR / "predict.py"), sequence]
    res1 = subprocess.run(cmd1, cwd=BASE_DIR, capture_output=True, text=True, check=True)

    pdb_path = BASE_DIR / "built_peptide.pdb"
    if not pdb_path.exists():
        raise RuntimeError("predict.py finished, but built_peptide.pdb was not created.")

    update_state(
        sequence=sequence,
        status="folded",
        message=f"Folded {sequence}",
        pdb=pdb_path.read_text(),
        log=res1.stdout,
        error="",
    )
    print("Pipeline finished! Updated webpage data.")
    return snapshot()


def find_arduino_port():
    """Automatically finds the connected Arduino USB port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        text = f"{p.description} {p.device} {p.hwid}".lower()
        # 2341 is Arduino's vendor id. The UNO R4 WiFi enumerates as "USB Serial Device".
        if "2341" in text or any(k in text for k in ("arduino", "usbmodem", "usbserial", "ch340", "ttyacm")):
            return p.device
    return None

def listen_to_arduino():
    """Background thread that auto-detects and reads from the Arduino."""
    while True:
        port = find_arduino_port()
        if not port:
            connected.clear()
            update_state(connected=False, message="No Arduino detected. Plug it in or close the Serial Monitor.")
            print("No Arduino detected. Plug it in or check USB cable...")
            time.sleep(3)
            continue

        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = BAUD_RATE
            ser.timeout = 1
            ser.dtr = False  # avoid resetting the board and wiping the sequence
            ser.rts = False
            ser.open()
            time.sleep(0.2)
            # The UNO R4 only transmits USB serial while the host raises DTR.
            # Raising it after open avoids the reset pulse and lets prints through.
            ser.dtr = True
            ser.rts = True
            try:
                print(f"Connected to Arduino on {port}!", flush=True)
                connected.set()
                update_state(
                    connected=True,
                    status="waiting",
                    message=f"Connected on {port}. Add a resistor and close the topper.",
                    error="",
                )
                time.sleep(0.3)
                ser.timeout = 0.2
                pending = b""
                while True:
                    while True:
                        try:
                            cmd = cmd_queue.get_nowait()
                        except queue.Empty:
                            break
                        ser.write(cmd.encode("utf-8"))
                        ser.flush()
                    waiting = ser.in_waiting
                    chunk = ser.read(waiting or 1)
                    if not chunk:
                        continue
                    pending += chunk
                    while b"\n" in pending:
                        raw, pending = pending.split(b"\n", 1)
                        line = raw.decode("utf-8", errors="ignore").strip("\r").strip()
                        if not line:
                            continue
                        print(f"Received from Arduino: {line}", flush=True)
                        handle_arduino_line(line)
            finally:
                ser.close()
        except (serial.SerialException, OSError) as e:
            connected.clear()
            update_state(connected=False, message=f"{port} is busy. Close the Arduino Serial Monitor.")
            print(f"Port {port} busy or disconnected (close Arduino Serial Monitor!): {e}", flush=True)
            time.sleep(3)



class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            html_file = BASE_DIR / "index.html"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(html_file.read_bytes())
        elif self.path == "/latest":
            self._send_json(200, snapshot())
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/fold":
            self._fold()
            return
        if self.path == "/board":
            self._board()
            return
        if self.path != "/run-predict":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length else "{}"
        data = json.loads(body)
        sequence = (data.get("letters") or data.get("sequence") or "").strip()

        try:
            result = run_pipeline(sequence)
            self._send_json(200, result)
        except subprocess.CalledProcessError as e:
            err = f"Script failed:\n{e.stderr or e.stdout}"
            update_state(status="error", error=err, message="Fold failed.")
            self._send_json(500, snapshot())
        except Exception as e:
            update_state(status="error", error=str(e), message="Fold failed.")
            self._send_json(500, snapshot())

    def _fold(self):
        """Tell the sketch to stop, then fold the sequence it has built."""
        if not fold_lock.acquire(blocking=False):
            self._send_json(409, {**snapshot(), "error": "Fold is already running."})
            return
        try:
            sequence = snapshot()["sequence"]
            if not sequence:
                update_state(error="No amino acids yet.")
                self._send_json(400, snapshot())
                return
            if not connected.is_set():
                update_state(error="Arduino is not connected.")
                self._send_json(400, snapshot())
                return

            fold_seen.clear()
            cmd_queue.put("fold\n")
            update_state(status="folding", message=f"Stopping the Arduino and folding {sequence}...", error="")
            fold_seen.wait(timeout=3)
            final_seq = snapshot()["sequence"] or sequence
            if not final_seq:
                self._send_json(400, {**snapshot(), "error": "Arduino stopped with an empty sequence."})
                return
            try:
                self._send_json(200, run_pipeline(final_seq))
            except subprocess.CalledProcessError as e:
                err = f"Script failed:\n{e.stderr or e.stdout}"
                update_state(status="error", error=err, message="Fold failed.")
                self._send_json(500, snapshot())
            except Exception as e:
                update_state(status="error", error=str(e), message="Fold failed.")
                self._send_json(500, snapshot())
        finally:
            fold_lock.release()

    def _board(self):
        """Reset, open-circuit cal, or one of the three range calibrations."""
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length else "{}"
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            self._send_json(400, {**snapshot(), "error": "Bad request."})
            return

        action = (data.get("action") or "").strip().lower()
        if action == "reset":
            if queue_command("r"):
                update_state(sequence="", status="waiting", message="Reset sent. Start with the loop open.")
            self._send_json(200 if not snapshot()["error"] else 400, snapshot())
            return

        if action == "calopen":
            ok = queue_command("calopen")
            self._send_json(200 if ok else 400, snapshot())
            return

        if action == "cal":
            try:
                rng = int(data.get("range"))
                ohms = float(data.get("ohms"))
            except (TypeError, ValueError):
                update_state(error="Enter the known resistance in ohms.")
                self._send_json(400, snapshot())
                return
            if rng not in (0, 1, 2) or not (ohms > 0):
                update_state(error="Range must be 0, 1, or 2 and ohms must be greater than 0.")
                self._send_json(400, snapshot())
                return
            ok = queue_command(f"cal {rng} {ohms:.1f}")
            self._send_json(200 if ok else 400, snapshot())
            return

        update_state(error="Unknown board action.")
        self._send_json(400, snapshot())

    def _send_json(self, status, payload):
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


if __name__ == "__main__":
    # Start USB listener in the background
    threading.Thread(target=listen_to_arduino, daemon=True).start()

    print("Server running at http://localhost:5000")
    ThreadingHTTPServer(("0.0.0.0", 5000), Handler).serve_forever()

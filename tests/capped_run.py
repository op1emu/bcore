"""Runs a command, keeping at most a limit of its output.

subprocess.run(capture_output=True) reads all of it into memory: a run that
loops printing fills the runner's memory within its timeout, and the runner
is shut down with nothing in the log to say which test it was.
"""

import subprocess
import threading
from dataclasses import dataclass
from typing import List, Optional

# Far more than any test prints.
OUTPUT_LIMIT = 16 << 20


@dataclass
class Capped:
    returncode: Optional[int]  # None when stopped: timed out or flooded
    stdout: str
    stderr: str
    timed_out: bool = False
    flooded: bool = False      # printed more than the limit, then killed


def run_capped(cmd: List[str], timeout: float, limit: int = OUTPUT_LIMIT) -> Capped:
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    kept = {"out": bytearray(), "err": bytearray()}
    total = [0]
    lock = threading.Lock()
    flooded = threading.Event()

    def drain(name: str, pipe) -> None:
        while True:
            chunk = pipe.read1(1 << 16)
            if not chunk:
                return
            with lock:
                room = max(limit - total[0], 0)
                kept[name] += chunk[:room]
                total[0] += len(chunk)
                over = total[0] > limit
            if over and not flooded.is_set():
                flooded.set()
                proc.kill()

    threads = [threading.Thread(target=drain, args=(name, pipe), daemon=True)
               for name, pipe in (("out", proc.stdout), ("err", proc.stderr))]
    for t in threads:
        t.start()
    timed_out = False
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
    finally:
        # Also on an interrupt: the run never outlives its caller.
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    # The pipes close with the process, unless a child of it holds them.
    for t in threads:
        t.join(timeout=5)
    stopped = timed_out or flooded.is_set()
    return Capped(None if stopped else proc.returncode,
                  kept["out"].decode(errors="replace"), kept["err"].decode(errors="replace"),
                  timed_out, flooded.is_set())

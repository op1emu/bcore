"""Runs a command, keeping at most a limit of its output.

subprocess.run(capture_output=True) reads all of it into memory: a run that
loops printing fills the runner's memory within its timeout, and the runner
is shut down with nothing in the log to say which test it was.

A run is over when its process has exited and its output pipes have closed:
a descendant that keeps them open keeps the run going. Both within one
timeout, as with subprocess.run; a run stopped (timeout, flood, interrupt) is
killed with every process of its session.
"""

import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import List, Optional

# Far more than any test prints.
OUTPUT_LIMIT = 16 << 20

# After the kill: for the readers to see the pipes close.
DRAIN_S = 1.0


@dataclass
class Capped:
    returncode: Optional[int]  # None when stopped: timed out or flooded
    stdout: str
    stderr: str
    timed_out: bool = False    # not exited, or its pipes not closed, in time
    flooded: bool = False      # printed more than the limit, then killed


def run_capped(cmd: List[str], timeout: float, limit: int = OUTPUT_LIMIT) -> Capped:
    deadline = time.monotonic() + timeout
    # A session of its own, so that a stop reaches its descendants (killpg).
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    kept = {"out": bytearray(), "err": bytearray()}
    total = [0]
    lock = threading.Lock()
    flooded = threading.Event()

    def kill() -> None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass  # every process of the session gone

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
                kill()

    threads = [threading.Thread(target=drain, args=(name, pipe), daemon=True)
               for name, pipe in (("out", proc.stdout), ("err", proc.stderr))]
    for t in threads:
        t.start()
    timed_out = False
    try:
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
        for t in threads:
            t.join(max(deadline - time.monotonic(), 0))
        timed_out = timed_out or any(t.is_alive() for t in threads)
    finally:
        # Also on an interrupt: nothing of the run outlives its caller.
        if timed_out or flooded.is_set() or proc.poll() is None or any(t.is_alive() for t in threads):
            kill()
        proc.wait()
        for t, pipe in zip(threads, (proc.stdout, proc.stderr)):
            t.join(DRAIN_S)
            if not t.is_alive():  # else a process outside the session holds it
                pipe.close()
    stopped = timed_out or flooded.is_set()
    return Capped(None if stopped else proc.returncode,
                  kept["out"].decode(errors="replace"), kept["err"].decode(errors="replace"),
                  timed_out and not flooded.is_set(), flooded.is_set())

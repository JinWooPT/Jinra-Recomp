"""Cliente local da gestão de recursos do Jinra-Recomp."""
import argparse
import json
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time

WINDOWS = os.name == "nt"


class Failed(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


class Bridge:
    """One connection: requests with ids, replies matched to them, events kept for watch"""

    def __init__(self, path, timeout=10.0):
        self.path, self.next_id, self.pending = path, 0, b""
        try:
            if WINDOWS:
                name = path if path.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + path
                self.sock = None
                deadline = time.monotonic() + timeout
                while True:   # (the bridge keeps one instance waiting: a busy pipe is waited for)
                    try:
                        self.pipe = open(name, "r+b", buffering=0)
                        break
                    except OSError as e:
                        if getattr(e, "winerror", 0) != 231 or time.monotonic() > deadline:   # ERROR_PIPE_BUSY
                            raise
                        time.sleep(0.02)
            else:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(timeout)
                self.sock.connect(path)
        except OSError as e:
            raise Failed("no game listens on %s (%s)" % (path, e.strerror or e), 2)

    def close(self):
        (self.sock or self.pipe).close()

    def send(self, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        if self.sock:
            self.sock.sendall(data)
        else:
            self.pipe.write(data)

    def line(self, timeout):
        """The next line as an object; None when the game went away"""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            try:
                if self.sock:
                    self.sock.settimeout(left)
                    chunk = self.sock.recv(65536)
                else:
                    chunk = self._pipe_read(left)
                    if chunk is None:
                        continue
            except socket.timeout:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            except OSError:
                chunk = b""
            if not chunk:
                return None
            self.pending += chunk
        text, self.pending = self.pending.split(b"\n", 1)
        return json.loads(text.decode("utf-8", "replace"))

    def _pipe_read(self, left):
        """(Windows) what the pipe holds, after waiting up to `left` seconds for some: None for nothing yet, b"" when the
        bridge's end closed. PeekNamedPipe and then a read of only what is there: a read that blocks would hold the
        handle's writes too (one synchronous handle)"""
        import ctypes
        import msvcrt
        handle = msvcrt.get_osfhandle(self.pipe.fileno())
        avail = ctypes.c_ulong(0)
        end = time.monotonic() + min(left, 0.05)
        while True:
            if not ctypes.windll.kernel32.PeekNamedPipe(ctypes.c_void_p(handle), None, 0, None, ctypes.byref(avail), None):
                return b""
            if avail.value:
                return self.pipe.read(avail.value)
            if time.monotonic() >= end:
                return None
            time.sleep(0.002)

    def request(self, cmd, timeout=None, **args):
        """The reply to one request (events that come meanwhile are dropped). Without a timeout the bridge's own default
        holds (for an input: 30 s plus its frames at 10 fps, so a long one can take hours): the client then waits as
        long as the bridge can (an hour at most), unless the connection ends"""
        self.next_id += 1
        rid = self.next_id
        req = {"id": rid, "cmd": cmd}
        req.update({k: v for k, v in args.items() if v is not None})
        if timeout is not None:
            req["timeout"] = timeout
        self.send(req)
        wait = (timeout if timeout is not None else 3600 if cmd in ("press", "stick", "sequence") else 60) + 10
        while True:
            msg = self.line(wait)
            if msg is None:
                if cmd == "quit":
                    return {"id": rid, "ok": True, "quitting": True}
                raise Failed("the game closed the connection", 2)
            if msg.get("id") == rid and "ok" in msg:
                return msg



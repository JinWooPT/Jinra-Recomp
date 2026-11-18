"""Controlo local da memória de recursos do Jinra-Recomp."""
import json
import ctypes
import msvcrt
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import re
import hashlib


ROOT = Path(__file__).resolve().parent
TOOLS = ROOT / 'control'


def checkpoint(game, bridge, folder, tag, scene):
    point = bridge.request('ram_resource_manager', timeout=2)
    if not point.get('ok') or not point.get('manager'):
        return False
    path = folder / f'queue-{tag}.json'
    subprocess.run([sys.executable, '-I', '-B', '-S', str(TOOLS / 'queue-snapshot.py'), str(game.pid),
                    str(point['manager']), str(path), '--identities'], timeout=12, check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    census = json.loads(path.read_text(encoding='utf-8'))
    if census.get('errors') or not census.get('nodes') or len(census['nodes']) != census['count']:
        return False
    state = bridge.request('status', timeout=2)
    if not playable(state, scene):
        return False
    for mark in (tag + '_play', tag + '_census_end'):
        with (folder / 'marks.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(label=mark, map=scene, qpc_ns=time.perf_counter_ns())) + '\n')
    return True


def playable(state, scene):
    game = state.get('game', {})
    return (scene and scene == state.get('map') and state.get('state') == 'play'
            and not state.get('stalled') and not game.get('loading') and not game.get('event'))


def manage(game, bridge, folder, log, protected):
    # The laboratory helper checks that the same node and allocation are still in
    # the queue after the transition. The native worker repeats its own checks.
    prepare = runpy.run_path(str(ROOT / 'jinra-ram-revisit-cohorts.py'))['prepare_revisit']
    catalogue = json.loads((ROOT / 'catalogue-across-orders-015.json').read_text(encoding='utf-8'))['by_zone']
    previous = None
    current = None
    stable_since = None
    number = 0
    active_scene = None
    last_status = 0.0
    while game.poll() is None:
        state = bridge.request('status', timeout=2)
        now = time.monotonic()
        scene = state.get('map')
        if active_scene and now - last_status >= 2:
            status = bridge.request('ram_resource_batch_status', timeout=2)
            last_status = now
            if status.get('state') in (3, 4):
                print(f'{active_scene}: {status.get("selected", 0)} recursos selecionados.', file=log, flush=True)
                active_scene = None
        if not playable(state, scene):
            stable_since = None
        elif scene != current or stable_since is None:
            current, stable_since = scene, now
        elif now - stable_since >= 8 and (previous is None or scene != previous[0]) and not active_scene:
            number += 1
            label = f'zone_{number:03d}'
            if checkpoint(game, bridge, folder, label, scene):
                if previous and previous[0] in catalogue and scene in protected:
                    old_scene, old_label = previous
                    tag = f'release-{number:03d}'
                    if prepare(folder, old_label, label, tag) and playable(bridge.request('status', timeout=2), scene):
                        start = time.perf_counter_ns()
                        result = bridge.request('ram_resource_batch_begin', timeout=2, observe=False,
                                                begin=start + 8_000_000_000, end=start + 38_000_000_000)
                        if result.get('ok'):
                            active_scene = scene
                            print(f'{old_scene} → {scene}: libertação agendada.', file=log, flush=True)
                previous = (scene, label)
                clear_buffers(folder, label)
            elif previous:
                clear_buffers(folder, previous[1])
            else:
                clear_buffers(folder, label + "_no_checkpoint")
            stable_since = time.monotonic()
        time.sleep(0.8)



class GameProcess:
    def __init__(self, pid):
        if os.getppid() != pid:
            raise RuntimeError("Only the parent game can start this worker")
        self.pid = pid
        dll = ctypes.WinDLL("kernel32", use_last_error=True)
        handle_type = ctypes.c_void_p
        dll.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        dll.OpenProcess.restype = handle_type
        dll.CloseHandle.argtypes = [handle_type]
        dll.WaitForSingleObject.argtypes = [handle_type, ctypes.c_ulong]
        dll.QueryFullProcessImageNameW.argtypes = [handle_type, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
        dll.GetNamedPipeServerProcessId.argtypes = [handle_type, ctypes.POINTER(ctypes.c_ulong)]
        self.api = dll
        self.handle = dll.OpenProcess(0x100000 | 0x1000, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        length = ctypes.c_ulong(32768)
        path = ctypes.create_unicode_buffer(length.value)
        root = ROOT.parent.resolve()
        allowed = {(root / "Jinra-Ocarina.exe").resolve(), (root / "bin" / "Jinra-Ocarina.exe").resolve()}
        if not dll.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(length)) or Path(path.value).resolve() not in allowed:
            self.close()
            raise RuntimeError("Unexpected game executable")

    def verify_server(self, bridge):
        server = ctypes.c_ulong()
        fd = msvcrt.get_osfhandle(bridge.pipe.fileno())
        return self.api.GetNamedPipeServerProcessId(fd, ctypes.byref(server)) and server.value == self.pid

    def poll(self):
        return None if self.api.WaitForSingleObject(self.handle, 0) == 258 else 0

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


def load_profile():
    manifest = json.loads((ROOT / "perfil.json").read_text(encoding="utf8"))
    # The protected list is fail-closed: a missing file must never broaden the cohort.
    for name, expected in manifest["inputs"].items():
        data = (ROOT / name).read_bytes().replace(b"\r\n", b"\n")
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Altered selective-resource input: " + name)
    return set(manifest["zones"])


def clear_buffers(folder, keep):
    for item in folder.glob("queue-zone_*.json"):
        if item.name != "queue-" + keep + ".json":
            item.unlink(missing_ok=True)
    for item in folder.glob("release-*-result.json"):
        item.unlink(missing_ok=True)
    marks = folder / "marks.jsonl"
    if marks.is_file():
        retained = [row for row in marks.read_text(encoding="utf8").splitlines()
                    if json.loads(row)["label"] in (keep + "_play", keep + "_census_end")]
        marks.write_text("".join(row + "\n" for row in retained), encoding="utf8")


def private_work(pid, name):
    location = Path(name).resolve(strict=True)
    buf = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetTempPathW(len(buf), buf):
        raise ctypes.WinError()
    if (location.parent != Path(buf.value).resolve() or not re.fullmatch(r"Jinra-015-" + str(pid) + "-[0-9a-f]{32}", location.name)
            or location.is_junction() or location.is_symlink()):
        raise ValueError("Invalid private session path")
    return location


def main():
    pid = int(sys.argv[1])
    game = GameProcess(pid)
    folder = private_work(pid, sys.argv[2])
    custom_log = os.environ.get("JINRA_RESOURCE_HELPER_LOG")
    stream = Path(custom_log).open("a", encoding="utf8", buffering=1) if custom_log else (folder / "estado.txt").open("w", encoding="utf8")
    bridge = None
    try:
        protected = load_profile()
        control = runpy.run_path(str(TOOLS / "jinra_ctl.py"))
        until = time.monotonic() + 180
        while game.poll() is None and time.monotonic() < until:
            try:
                bridge = control["Bridge"](os.environ["HOS_CONTROL"], timeout=1)
            except control["Failed"]:
                time.sleep(.4)
                continue
            if not game.verify_server(bridge):
                raise RuntimeError("Resource bridge belongs to a different process")
            print("Gestor ligado ao processo do jogo.", file=stream, flush=True)
            manage(game, bridge, folder, stream, protected)
            break
    except Exception as error:
        print(str(error), file=stream, flush=True)
    finally:
        if bridge is not None:
            bridge.close()
        # A pending batch is owned by the game until it exits, even if the
        # helper detected a temporary connection failure.
        game.api.WaitForSingleObject(game.handle, 0xffffffff)
        game.close()
        stream.close()
        try:
            for item in folder.iterdir():
                safe = (item.name in ("pending-resources.bin", "marks.jsonl", "estado.txt") or
                        re.fullmatch(r"(queue-zone_\d+|release-\d+-result)\.json", item.name))
                if safe and item.is_file() and not item.is_symlink():
                    item.unlink()
            folder.rmdir()
        except OSError:
            pass


if __name__ == "__main__":
    main()

import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct
import sys
import time


BASE = 0x100000000000
GUEST_SIZE = 1 << 39
PAGE = 4096
POINTER_FIELDS = (0x18, 0x20, 0x30, 0x60, 0x68, 0xc0, 0xc8, 0x390)


class GuestReader:
    def __init__(self, pid):
        api = C.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = (W.DWORD, W.BOOL, W.DWORD)
        api.OpenProcess.restype = W.HANDLE
        api.ReadProcessMemory.argtypes = (W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t))
        api.ReadProcessMemory.restype = W.BOOL
        api.CloseHandle.argtypes = (W.HANDLE,)
        self.api = api
        self.handle = api.OpenProcess(0x410, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.pages = {}

    def read(self, addr, length):
        if addr < 0x10000 or addr + length > GUEST_SIZE:
            raise ValueError(f"Address outside the guest window: {addr:x}")
        data = C.create_string_buffer(length)
        size = C.c_size_t()
        if not self.api.ReadProcessMemory(self.handle, BASE + addr, data, length, C.byref(size)) or size.value != length:
            raise C.WinError(C.get_last_error())
        return data.raw

    def cached(self, addr, length):
        page, offset = divmod(addr, PAGE)
        if offset + length > PAGE:
            return self.read(addr, length)
        if page not in self.pages:
            self.pages[page] = self.read(page * PAGE, PAGE)
        return self.pages[page][offset:offset + length]


def dump(reader, manager, include_identity):
    start = time.perf_counter_ns()
    header = reader.read(manager + 0x50, 0x18)
    tail, current, count, offset = struct.unpack("<QQii", header)
    sentinel = manager + 0x50
    if not 0 <= count <= 25000 or not 0 <= offset <= 0x400 or offset % 8:
        raise ValueError("Unexpected layout of the resource list")
    nodes = []
    previous = sentinel
    while current != sentinel and len(nodes) < count:
        link = reader.cached(current, 16)
        prev, following = struct.unpack("<QQ", link)
        if prev != previous or following == current:
            raise ValueError("The resource list changed during the read")
        item = current - offset
        fields = reader.cached(item, 0xd0)
        owner = struct.unpack("<Q", reader.cached(item + 0x390, 8))[0]
        nodes.append([item, fields[2], fields[5], struct.unpack_from("<I", fields, 0x10)[0],
                      struct.unpack_from("<I", fields, 0x90)[0],
                      *[struct.unpack_from("<Q", fields, field)[0] for field in POINTER_FIELDS[:-1]], owner])
        if include_identity:
            name_id = None
            state8_name_id = None
            state8_suffix = None
            if nodes[-1][3] in (8, 11) and not nodes[-1][4] and owner:
                try:
                    name_ptr, room = struct.unpack("<Qi", reader.cached(item + 0x2e0, 12))
                    if name_ptr >= 0x10000 and 1 < room <= 512:
                        value = reader.cached(name_ptr, room).split(b"\0", 1)[0]
                        if value and all(0x20 <= c < 0x7f for c in value):
                            digest = hashlib.sha256(value).hexdigest()[:24]
                            if nodes[-1][3] == 11:
                                name_id = digest
                            else:
                                state8_name_id = digest
                                suffix = value.rsplit(b"/", 1)[-1].rsplit(b".", 1)
                                if len(suffix) == 2 and 1 <= len(suffix[1]) <= 12 and suffix[1].isalnum():
                                    state8_suffix = suffix[1].decode("ascii").lower()
                except (OSError, ValueError):
                    pass
            nodes[-1].extend((struct.unpack_from("<I", fields, 0xc)[0], name_id,
                              state8_name_id, state8_suffix))
        previous = current
        current = following
    if len(nodes) != count or current != sentinel or previous != tail or reader.read(manager + 0x50, 0x18) != header:
        raise ValueError("The resource list changed during the read")
    columns = ["item", "flag_2", "flag_5", "state_10", "field_90", *[f"field_{field:x}" for field in POINTER_FIELDS]]
    if include_identity:
        columns.extend(("flag_c", "resource_id", "state8_candidate_id", "state8_suffix"))
    return dict(manager=manager, count=count, link_offset=offset, duration_ms=(time.perf_counter_ns() - start) / 1e6,
                columns=columns, nodes=nodes)


def main():
    pid, manager, output, *options = sys.argv[1:]
    reader = GuestReader(int(pid))
    errors = []
    try:
        for _ in range(3):
            reader.pages.clear()
            try:
                result = dump(reader, int(manager), options == ["--identities"])
            except (ValueError, OSError) as error:
                errors.append(str(error))
            else:
                break
        else:
            result = dict(errors=errors)
    finally:
        reader.api.CloseHandle(reader.handle)
    Path(output).write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")


if __name__ == "__main__":
    main()

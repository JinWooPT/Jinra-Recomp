"""Prepare the three native resource hooks from this installation's own translated C."""
from pathlib import Path
import hashlib
import json
import os
import re

SITES = ("f_801ae480", "f_801b4260", "f_801b9180")
IMAGE_ID = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"


def replace_once(text, marker, replacement):
    if text.count(marker) != 1:
        raise ValueError("Unexpected instruction layout in the resource hook")
    return text.replace(marker, replacement)


def generate(gen):
    if ('"' + IMAGE_ID + '"') not in (gen / "hos_image.c").read_text(encoding="utf8"):
        raise ValueError("Resource management requires the supported game build")
    signatures = {name: "void " + name + "(Ctx *__restrict A64_NOESC c) {" for name in SITES}
    found = {}
    # The usual split puts these functions in two chunks. Other split profiles
    # are supported by inspecting the remaining local chunks as needed.
    preferred = [gen / "code_001.c", gen / "code_hot_2.c"]
    chunks = sorted(gen.glob("code_*.c"))
    for chunk in dict.fromkeys(preferred + chunks):
        if not chunk.is_file():
            continue
        text = chunk.read_text(encoding="utf8")
        for name, signature in signatures.items():
            if name in found or signature not in text:
                continue
            match = re.search("^" + re.escape(signature) + r".*?^\}", text, re.M | re.S)
            if not match:
                raise ValueError("Incomplete local translation of " + name)
            found[name] = match.group().replace("void " + name, "void __wrap_" + name, 1)
        if len(found) == len(SITES):
            break
    if len(found) != len(SITES):
        raise ValueError("Local translation lacks the supported resource functions")
    functions = []
    for name, delta, count in ((SITES[0], "9", 2), (SITES[1], "10", 1)):
        lines = found[name].splitlines()
        changed = 0
        for index, line in enumerate(lines):
            atomic = "__atomic_fetch_add(_p, (uint32_t)c->x[" + delta + "], __ATOMIC_SEQ_CST)"
            if atomic in line and "_b=144ULL;" in " ".join(lines[max(0, index-4):index]):
                lines[index] = replace_once(line, "A64_MARK(_a, 4);", "A64_MARK(_a, 4); if (!_o) jinra_record_reference_acquire(_a - 0x90);")
                changed += 1
        if changed != count:
            raise ValueError("Resource reference bit layout changed")
        body = "\n".join(lines)
        if name == SITES[0]:
            header = signatures[name].replace("void f_", "void __wrap_f_")
            body = replace_once(body, header, header + "\n    const uint64_t request_lr = c->x[30];")
            for token, code in (("    recomp_store32_sp(c,c->x[19] + 144,(uint64_t)0);", "    jinra_state8_node_event('b', c->x[19], 0, request_lr);"),
                                ("    recomp_store64(c,c->x[19] + 912,c->x[8]);", "    if (c->x[8]) jinra_state8_node_event('k', c->x[19], c->x[8], request_lr);")):
                body = replace_once(body, token, token + "\n" + code)
        functions.append(body)
    head = "    c->x[8]=recomp_load64(c,c->x[20] + 88);"
    label = "L_801baee8:;\n"
    functions.append(replace_once(found[SITES[2]], label + head, label +
                     "    c->x[8]=jinra_resource_select_link(c->x[20], recomp_load64(c,c->x[20] + 88));\n"
                     "    if (!c->x[8]) goto L_801bb340;"))
    combined = "\n\n".join(functions)
    names = sorted(set(re.findall(r"\bf_[a-f0-9]+\b", combined)).union(SITES))
    return ('#include "a64.h"\nextern void jinra_record_reference_acquire(uint64_t node);\n'
            'extern uint64_t jinra_resource_select_link(uint64_t manager, uint64_t head);\n'
            'extern void jinra_state8_node_event(char, uint64_t, uint64_t, uint64_t);\n' +
            "\n".join("void " + name + "(Ctx *A64_NOESC);" for name in names) + "\n\n" + combined + "\n")


def prepare(gen, kept):
    """Return None only if the ordinary builder needs to regenerate this PC's local lift."""
    gen = Path(gen)
    cache = gen.parent / "jinra-resource-hooks"
    work = cache / "resource-hooks.c"
    mark = cache / "source.json"
    tool = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if kept:
        try:
            stamp = json.loads(mark.read_text(encoding="ascii"))
            if (stamp["generator"] == tool and stamp["local_lift"] == (gen / ".lift_key").read_text().splitlines()[0]
                    and stamp["sha256"] == hashlib.sha256(work.read_bytes()).hexdigest()):
                return str(work)
        except (OSError, ValueError, KeyError, IndexError):
            pass
        return None
    data = generate(gen).encode("utf8")
    cache.mkdir(exist_ok=True)
    temp = work.with_suffix(".new")
    temp.write_bytes(data)
    os.replace(temp, work)
    keyfile = gen / ".lift_key"
    keys = keyfile.read_text().splitlines() if keyfile.is_file() else []
    stamp = dict(generator=tool, local_lift=keys[0] if keys else "", sha256=hashlib.sha256(data).hexdigest())
    tmp = mark.with_suffix(".new")
    tmp.write_text(json.dumps(stamp), encoding="ascii")
    os.replace(tmp, mark)
    return str(work)

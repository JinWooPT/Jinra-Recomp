import json
import os
from pathlib import Path
import runpy
import struct
import time


def census(folder, name):
    data = json.loads((folder / f"queue-{name}.json").read_text())
    rows = [dict(zip(data["columns"], row)) for row in data.get("nodes", [])[1:]]
    return {node["resource_id"]: node for node in rows if node["resource_id"]}


def available(folder, cohort, source="zone_03", destination="zone_04"):
    prior, current = census(folder, source), census(folder, destination)
    for resource in cohort:
        identity = resource["resource_id"]
        if identity not in prior or identity not in current:
            continue
        a, b = prior[identity], current[identity]
        if all(a[key] == b[key] for key in ("item", "field_30", "field_390")):
            yield b["item"], b["field_390"], resource["bytes"]


def write_candidates(folder, candidates, tag):
    candidates.sort(reverse=True)
    picked = {}
    for size, node, base, origin, zone in candidates:
        if node not in picked:
            picked[node] = (base, size, origin, zone)
        if len(picked) == 2048:
            break
    if not picked:
        raise RuntimeError("The earlier zones supplied no eligible resources")
    with (folder / "pending-resources.bin").open("wb") as stream:
        for node, (base, size, origin, _) in picked.items():
            stream.write(struct.pack("<QQQQ", node, base, size, origin))
    (folder / f"{tag}-result.json").write_text(json.dumps(
        [dict(node=node, base=base, proposed_bytes=size, zone=zone) for node, (base, size, _, zone) in picked.items()],
        indent=2) + "\n")



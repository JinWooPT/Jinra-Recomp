"""Apply the Windows host's unified diffs without requiring Git in the player's setup."""
from pathlib import Path
import re

HEADER = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def apply(source, patch):
    before = source.splitlines(keepends=True)
    output = []
    position = 0
    hunk = False
    for line in patch.splitlines(keepends=True):
        if not hunk and line.startswith(("--- ", "+++ ")):
            continue
        if line.startswith("@@"):
            m = HEADER.match(line)
            if not m:
                raise ValueError("Malformed Windows source patch")
            start = max(0, int(m[1]) - 1)
            if start < position or start > len(before):
                raise ValueError("Windows source patch does not fit")
            output.extend(before[position:start])
            position = start
            hunk = True
            continue
        if not hunk or line[0] not in " +-":
            raise ValueError("Unrecognised Windows source patch line")
        if line[0] != "+":
            if position >= len(before) or before[position] != line[1:]:
                raise ValueError("Windows source patch and engine revision differ")
            position += 1
        if line[0] != "-":
            output.append(line[1:])
    output.extend(before[position:])
    return "".join(output)


def sources(host, destination, native):
    """Build the platform variants in the local build directory; keep the base sources intact."""
    host, destination = Path(host), Path(destination)
    patches = host / "windows_profile"
    destination.mkdir(parents=True, exist_ok=True)
    variants = {}
    for patch in sorted(patches.glob("*.c.patch")):
        name = patch.name[:-6]
        original = host / name
        generated = apply(original.read_text(encoding="utf8") if original.is_file() else "", patch.read_text(encoding="utf8"))
        path = destination / name
        if not path.is_file() or path.read_text(encoding="utf8") != generated:
            path.write_text(generated, encoding="utf8", newline="\n")
        variants[name] = str(path)
    new_names = sorted(name for name in variants if not (host / name).is_file())
    for name in new_names:
        if name != "jinra_gap.c":
            raise ValueError("Unexpected additional Windows host unit: " + name)
    results = [variants.get(Path(path).name, path) for path in native]
    results.extend(variants[name] for name in new_names)
    return results

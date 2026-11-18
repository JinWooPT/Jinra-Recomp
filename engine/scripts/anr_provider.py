"""Install the pinned optional Windows ANR provider next to a Jinra game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import urllib.request
import zipfile


ASSETS = Path(__file__).resolve().parents[1] / "assets" / "anr"
URL = "https://github.com/apex-compute/ANR/releases/download/FG-V.B18/anr-fg-V.B18-addon.zip"
ARCHIVE_SHA256 = "6dfce1ce2240f6ce8d820a7eb66b4d7d349655e9238c30df820297d70b9963f3"
PREFIX = "Apex Neural Rendering FG/"
MAX_DOWNLOAD = 16 << 20
PRIOR_PROVIDER_SHA256 = "be9a9c4f21c89cbae993f1c2f5df0685e3f530bf9f2e565e22958dd062c9c12c"
PRIOR_BRIDGE_SHA256 = "8e747ffbffedeec9fccc4a5ab381a1a0e1335325df8c43043c4b854b4621b608"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_provider(folder, expected, *, personal_settings=False):
    for name, checksum in expected.items():
        path = folder / name
        if not path.is_file():
            raise ValueError(f"Missing or incompatible ANR component: {name}")
        if personal_settings and name == "dlssg/nr_fg.ini":
            continue
        actual = digest(path)
        if actual != checksum and not (personal_settings and name == "dlssg/nvngx_dlssg.dll"
                                        and actual == PRIOR_PROVIDER_SHA256):
            raise ValueError(f"Incompatible ANR component: {name}")


def install(game, archive):
    if not game.is_dir():
        raise ValueError(f"Game directory does not exist: {game}")
    expected = json.loads((ASSETS / "provider-files.json").read_text(encoding="utf8"))
    bridge = ASSETS / "jinra_anr_bridge.dll"
    target = game / "anr"
    if not bridge.is_file():
        raise ValueError("The Jinra ANR bridge is missing from the kit.")
    accepted_bridges = {digest(bridge), PRIOR_BRIDGE_SHA256}

    def verify_existing():
        installed_bridge = target / bridge.name
        if not installed_bridge.is_file() or digest(installed_bridge) not in accepted_bridges:
            raise ValueError("An incompatible ANR bridge already exists. Move the anr folder aside before installing.")
        verify_provider(target, expected, personal_settings=True)

    if target.exists():
        verify_existing()
        print("ANR FG V.B18 is already installed.")
        return
    # Staging under the installation ensures the final directory rename stays on one volume.
    with tempfile.TemporaryDirectory(prefix=".jinra-anr-", dir=game) as tmp:
        work = Path(tmp)
        if archive is None:
            archive = work / "provider.zip"
            with urllib.request.urlopen(URL, timeout=35) as response, archive.open("xb") as output:
                received = 0
                while chunk := response.read(256 << 10):
                    received += len(chunk)
                    if received > MAX_DOWNLOAD:
                        raise ValueError("ANR download exceeds the size expected for V.B18.")
                    output.write(chunk)
        if digest(archive) != ARCHIVE_SHA256:
            raise ValueError("Wrong SHA-256 for the ANR FG V.B18 archive.")
        folder = work / "complete"
        folder.mkdir()
        with zipfile.ZipFile(archive) as pack:
            for name in expected:
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                with pack.open(PREFIX + name) as src, path.open("xb") as dst:
                    shutil.copyfileobj(src, dst, 256 << 10)
            for filename in ("README.txt", "nr_fg_package.json"):
                (folder / filename).write_bytes(pack.read(filename))
        verify_provider(folder, expected)
        shutil.copy2(bridge, folder / bridge.name)
        try:
            folder.rename(target)
        except OSError:
            if not target.exists():
                raise
            verify_existing()
    print("ANR FG V.B18 is ready.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--archive", type=Path, help="Previously downloaded official ANR FG V.B18 add-on ZIP.")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This optional bridge requires Windows.")
    try:
        install(args.game.resolve(), args.archive)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"ANR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

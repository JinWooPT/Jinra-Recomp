"""Build the Windows launcher and its self-contained, offline package."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent
LOCAL = ROOT / ".local"
TOOLCHAIN = "llvm-mingw-20261104-ucrt-x86_64"
MANIFEST = ROOT / "launcher" / "tools.json"
APP_SHA = "2d42d666e5a8f7d8cc32516bb466e02e1807f7d7c5e1e25256751e0355bfd47a"
THIRD_PARTY_SHA = "27fa239ccfefbf00f46917d481d8ff02981f8bcb42efaddf57e3c15abcc28780"


def checked(name, sha):
    path = LOCAL / f"{name}.zip"
    with path.open("rb") as file:
        if hashlib.file_digest(file, "sha256").hexdigest() != sha:
            raise ValueError(f"SHA-256 inválido em {path}")
    return path


def copy_package(zip_path, prefix, dest, *, packaged_tools=False, only=None):
    with zipfile.ZipFile(zip_path) as archive:
        if packaged_tools:
            roots = {entry.filename.split('/', 1)[0] for entry in archive.infolist() if not entry.is_dir()}
            if len(roots) != 1:
                raise ValueError(f"Estrutura inesperada em {zip_path}")
            prefix = roots.pop() + "/tools/" + prefix
        for entry in archive.infolist():
            if entry.is_dir() or not entry.filename.startswith(prefix):
                continue
            relative = entry.filename.removeprefix(prefix)
            if only is not None and relative not in only:
                continue
            path = (dest / (only[relative] if only is not None else relative)).resolve()
            if not path.is_relative_to(dest.resolve()):
                raise ValueError(f"Entrada fora do destino em {zip_path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)


def zip_tree(path, folder):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=3) as archive:
        for file in sorted(folder.rglob("*")):
            if file.is_file():
                archive.write(file, file.relative_to(folder).as_posix())


def run(*args, cwd=ROOT):
    subprocess.run(args, cwd=cwd, check=True)


def package(stage):
    print("A verificar os pacotes de ferramentas…", flush=True)
    tools = json.loads(MANIFEST.read_text(encoding="utf-8"))
    archives = {tool["name"]: checked(tool["name"], tool["sha256"]) for tool in tools}
    app = checked("app", APP_SHA)
    third_party = checked("third_party", THIRD_PARTY_SHA)
    kit = stage / "kit"
    engine = kit / "engine"
    shutil.copytree(ROOT / "engine", engine, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".git", "work"))
    shutil.copy2(ROOT / "launcher/Assets/Jinra.png", engine / "assets/jinra.png")
    shutil.copy2(MANIFEST, kit / "tools.json")
    for tool in tools:
        copy_package(archives[tool["name"]], tool["prefix"], kit / "tools" / tool["name"],
                     packaged_tools=tool.get("packaged_tools", False))
    copy_package(third_party, "third_party/", engine / "third_party", packaged_tools=True, only={
        name: name for name in ("a64-kit/lift/a64emit.dll", "a64-kit/lift/a64emit.dll.sha1", "a64-kit/lift/a64emit.lib",
                                "a64-kit/tools/lld-link.exe", "sdl2/SDL2.dll", "sdl2/SDL3.dll",
                                "zbic/zbic.dll", "zbic/zbic.lib")})
    with zipfile.ZipFile(app) as archive:
        profiles = [entry.filename.rsplit('/', 1)[-1] for entry in archive.infolist()
                    if '/tools/work/' in entry.filename and entry.filename.endswith('_pieces.prof')]
    if len(profiles) != 1:
        raise ValueError(f"Perfil de compilação em falta em {app}")
    copy_package(app, "work/", engine / "work", packaged_tools=True, only={
        profiles[0]: "jinra_pieces.prof", "hot_funcs.txt": "hot_funcs.txt", "hot_order.txt": "hot_order.txt",
        "image/chunks.txt": "image/chunks.txt"})
    shutil.copy2(ROOT / "native/vendor/nxformat/LICENSE", engine / "NXFORMAT-LICENSE")
    compiler = kit / "tools" / "llvm" / "bin"
    resource = stage / "jinra.res"
    run(compiler / "windres.exe", "-i", "jinra.rc", "-O", "res", "-o", resource, cwd=ROOT / "native")
    shutil.copy2(resource, engine / "work" / "jinra.res")
    zstd = engine / "third_party" / "zbic"
    zstd_object = stage / "zstd.o"
    run(compiler / "clang.exe", "-O3", "-c", zstd / "zstd.c", "-o", zstd_object)
    run(compiler / "clang++.exe", "-std=c++20", "-O2", "-static", "-municode",
        "-I" + str(ROOT / "native/vendor/nxformat"), "-I" + str(zstd), ROOT / "native/nx_import.cpp",
        zstd_object, "-o", engine / "jinra-nx.exe")
    stub = stage / "stub.exe"
    run(compiler / "clang++.exe", "-std=c++20", "-O2", "-static", "-municode", "-mwindows",
        ROOT / "native/bootstrap.cpp", resource, "-o", stub)
    run(compiler / "clang++.exe", "-std=c++20", "-O2", "-static", "-municode", "-mwindows",
        ROOT / "native/play.cpp", resource, "-o", stage / "jinra-play.exe")
    kit_zip = stage / "kit.zip"
    print("A empacotar o motor e as ferramentas…", flush=True)
    zip_tree(kit_zip, kit)
    publish = stage / "publish"
    print("A compilar a janela do Jinra-Recomp…", flush=True)
    feed = LOCAL / "nuget-feed"
    restore = [f"-p:RestoreSources={feed}"] if feed.is_dir() else []
    run("dotnet", "publish", ROOT / "launcher/Jinra-Recomp.csproj", "-c", "Release", "-r", "win-x64",
        "--self-contained", "true", "-p:NuGetAudit=false", "-p:RestoreIgnoreFailedSources=true",
        *restore, f"-p:Payload={kit_zip}", "-o", publish)
    shutil.copy2(stage / "jinra-play.exe", publish / "jinra-play.exe")
    (publish / "Jinra-Recomp.UI.pdb").unlink(missing_ok=True)
    shutil.copy2(ROOT / "LICENSE", publish / "LICENSE.txt")
    packed = stage / "launcher.zip"
    print("A montar o executável final…", flush=True)
    zip_tree(packed, publish)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    final = dist / "Jinra-Recomp.exe"
    partial = dist / "Jinra-Recomp.exe.partial"
    with partial.open("wb") as output:
        for file in (stub, packed):
            with file.open("rb") as source:
                shutil.copyfileobj(source, output, 1024 * 1024)
        output.write(struct.pack("<Q8s", packed.stat().st_size, b"JINRAPK1"))
    partial.replace(final)
    with final.open("rb") as file:
        sha256 = hashlib.file_digest(file, "sha256").hexdigest()
    (dist / "SHA256SUMS.txt").write_text(f"{sha256}  {final.name}\n", encoding="ascii")
    print(f"{final}\nSHA-256 {sha256}")


if __name__ == "__main__":
    LOCAL.mkdir(exist_ok=True)
    os.environ.setdefault("DOTNET_GENERATE_ASPNET_CERTIFICATE", "false")
    with tempfile.TemporaryDirectory(dir=LOCAL, prefix="package-") as directory:
        package(Path(directory))

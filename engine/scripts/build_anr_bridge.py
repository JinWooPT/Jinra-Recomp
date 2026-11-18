"""Build the Windows x64 ANR bridge from an x64 Native Tools command prompt."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ngx-sdk", type=Path, required=True,
                        help="NGX SDK directory containing include/nvsdk_ngx_vk.h.")
    parser.add_argument("--vulkan-headers", type=Path, required=True,
                        help="Vulkan-Headers directory containing include/vulkan/vulkan.h.")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compiler = shutil.which("cl")
    if not compiler:
        parser.error("Open an x64 Native Tools Command Prompt with the MSVC C++ workload installed.")
    vk = (args.vulkan_headers / "include").resolve()
    ngx = (args.ngx_sdk / "include").resolve()
    if not (vk / "vulkan/vulkan.h").is_file() or not (ngx / "nvsdk_ngx_vk.h").is_file():
        parser.error("The selected SDK folders do not contain the required headers.")
    source = Path(__file__).resolve().parents[1] / "gpu" / "vk"
    out = args.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    # The virtual parameter interface used by the provider requires the MSVC ABI.
    with tempfile.TemporaryDirectory(prefix="jinra-anr-", dir=out.parent) as temporary:
        work = Path(temporary)
        built = work / "jinra_anr_bridge.dll"
        subprocess.run([compiler, "/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/LD",
                        "/I" + str(ngx), "/I" + str(vk), "/I" + str(source),
                        str(source / "anr" / "jinra-anr-bridge.cpp"), "/link",
                        "/MACHINE:X64", "/Brepro", "/OUT:" + str(built)], cwd=work, check=True)
        built.replace(out)


if __name__ == "__main__":
    main()

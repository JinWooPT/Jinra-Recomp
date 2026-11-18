# ANR frame generation on Windows

Jinra can load Apex Neural Rendering FG V.B18 through a small Windows x64 adapter. The renderer uses the Vulkan interface in the NVIDIA NGX SDK. The Jinra adapter is built from `gpu/vk/anr/jinra-anr-bridge.cpp` and follows the repository license. The SDK has its own license in `LICENSE.txt` in the downloaded SDK. The provider and neural model files come from Apex Compute's optional download. [ANR FG V.B18 release](https://github.com/apex-compute/ANR/releases/tag/FG-V.B18)

## Install the optional provider

Close the game and use a Jinra renderer built with NVIDIA NGX support. From the engine directory, run the following command with the game installation folder.

```powershell
python scripts/anr_provider.py --game "D:\Games\Jinra-Recomp"
```

For an offline installation, add `--archive` followed by the path to the official `anr-fg-V.B18-addon.zip`. The importer verifies the official archive and each of its 19 provider files. An upgrade preserves compatible existing installations and their local `nr_fg.ini`.

In the graphics menu, select a temporal upscaler such as FSR 3.1, turn HDR off, choose 2× frame generation and select **ANR FG (Beta, 2×)**. The menu shows when the provider is generating frames and when the built-in engine takes over.

## Build the Jinra adapter

Use the x64 Native Tools Command Prompt for Visual Studio with the MSVC C++ workload. The build uses the NVIDIA NGX and Vulkan headers from the local SDK installations. The result uses the MSVC ABI and the static C runtime.

```powershell
python scripts/build_anr_bridge.py --ngx-sdk "<NGX SDK directory>" --vulkan-headers "<Vulkan-Headers directory>" --output assets/anr/jinra_anr_bridge.dll
```

The renderer itself builds with `nx2/build.py --standalone` as part of the normal Jinra recompilation. Set `JINRA_NGX_SDK` to the NGX SDK root for ANR support. To rebuild only the renderer, set `RX_VK_ONLY=1` for that build.

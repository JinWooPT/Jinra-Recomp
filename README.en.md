# Jinra-Recomp

English · [Português](README.md)

Local recompilation tool for **The Legend of Zelda: Ocarina of Time** for Nintendo Switch 2, version 1.1.1 (`e0f3ccfa`). It runs on Windows x64 with Vulkan.

## Getting started

1. Download `Jinra-Recomp.exe` from the [latest release](https://github.com/JinWooPT/Jinra-Recomp/releases/latest). Open it and choose **English** or **Português** in the sidebar.
2. Select your copy of the game as an `.nx` or `.nup` file, or as a folder containing `exefs` and `romfs`. **Verify game copy** checks the version.
3. Choose an empty destination folder and select **Build game**. Allow about 60 GB of free space on an SSD for the build and game files.
4. After building, open `Jinra-Ocarina.exe` on the desktop. It opens the installed launcher with **Play**, **Updates**, **Save RAM while playing**, and **Measure smoothness and RAM while playing**. You can delete the setup file you downloaded.

## Updates and saves

Select **Updates** to check for the latest stable release. Jinra verifies the download size and SHA-256. If the engine changes, the launcher updates the existing installation using the extracted game files and compatible build objects. The 0.1.6 to 0.1.7 update replaces the desktop launcher and reuses the existing game build.

In your emulator, open the Ocarina of Time save folder. In the launcher, choose **Import saves** and select the folder that directly contains `slot_00`, `slot_01`, and the other slot folders. You can select it before the first build or import into an existing installation while the game is closed. Jinra imports all valid slots, replaces matching slot numbers, and keeps the previous saves in `saves-backup`.

## Graphics and diagnostics

- **NVIDIA DLSS 4.5** is optional at build time. In **F1 → Picture**, choose **Upscaling method → NVIDIA DLSS**. The reconstruction model picker offers **K** for DLSS 4 and **M/L** for DLSS 4.5. The frame generation settings are under **Performance**.
- **AMD FSR 3.1** is included. **AMD FSR 4** is optional and appears in the game on compatible graphics cards. To use FSR frame generation, select **AMD FSR 3.1 (2x)** as the frame generation engine and set the multiplier to **2x**.
- **Ctrl+V** toggles the performance overlay. Use **F1 → Interface** to set its mode and position or change the language of the in-game settings.
- **Measure frame times and RAM while playing** writes three CSV files per session to `diagnostico` and disables frame generation during measurement. Keep the launcher open. **Save RAM while playing** skips loading the Vulkan cache, which can increase stutter during shader compilation.
- Game logs are in `logs`. Use **F1 → Diagnostics → Save diagnostic report** to create a shareable report in `logs/reports`.

## Build the installer

`python package.py` builds `dist/Jinra-Recomp.exe`. It requires Python 3.12 or later and the .NET 10 SDK with the Windows x64 self-contained runtime packages. The packager verifies the input ZIP files in `.local` against the hashes in `package.py` and `launcher/tools.json`.

The project and engine integration are licensed under GPL-3.0-or-later. Bundled components carry their respective licenses alongside the source.

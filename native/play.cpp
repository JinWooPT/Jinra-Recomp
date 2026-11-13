// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        std::wstring self(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (!length || length == self.size()) throw std::runtime_error("module path");
        self.resize(length);
        std::ifstream file(fs::path(self), std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size < 12) throw std::runtime_error("trailer");
        file.seekg(-12, std::ios::end);
        uint32_t bytes = 0;
        char marker[8]{};
        file.read(reinterpret_cast<char*>(&bytes), sizeof bytes);
        file.read(marker, sizeof marker);
        if (std::string_view(marker, 8) != "JINRAPLY" || !bytes || bytes > 65534 || bytes % 2 ||
            size < static_cast<std::streamoff>(12 + bytes)) throw std::runtime_error("target size");
        std::wstring target(bytes / sizeof(wchar_t), L'\0');
        file.seekg(-static_cast<std::streamoff>(12 + bytes), std::ios::end);
        if (!file.read(reinterpret_cast<char*>(target.data()), bytes)) throw std::runtime_error("target");
        const fs::path exe(target);
        if (!exe.is_absolute() || exe.filename() != L"Jinra-Ocarina.exe" || !fs::is_regular_file(exe))
            throw std::runtime_error("game path");
        const auto directory = exe.parent_path().wstring();
        std::wstring command = L"\"" + target + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            directory.c_str(), &startup, &process)) throw std::runtime_error("launch");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    } catch (...) {
        MessageBoxW(nullptr, L"N\u00e3o foi poss\u00edvel abrir o Zelda. Abre o Jinra-Recomp para verificar a pasta do jogo.",
                    L"Jinra-Ocarina", MB_OK | MB_ICONERROR);
        return 1;
    }
}

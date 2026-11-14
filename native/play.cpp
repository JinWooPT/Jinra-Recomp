// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include "command_line.h"
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
        if (std::string_view(marker, 8) != "JINRALCH" || !bytes || bytes > 131068 || bytes % 2 ||
            size < static_cast<std::streamoff>(12 + bytes)) throw std::runtime_error("target size");
        std::wstring target(bytes / sizeof(wchar_t), L'\0');
        file.seekg(-static_cast<std::streamoff>(12 + bytes), std::ios::end);
        if (!file.read(reinterpret_cast<char*>(target.data()), bytes)) throw std::runtime_error("target");
        const auto separator = target.find(L'\0');
        if (separator == std::wstring::npos || target.find(L'\0', separator + 1) != std::wstring::npos)
            throw std::runtime_error("launcher data");
        const fs::path exe(target.substr(0, separator));
        const fs::path game(target.substr(separator + 1));
        if (!exe.is_absolute() || exe.filename() != L"Jinra-Recomp.UI.exe" || !fs::is_regular_file(exe) ||
            !game.is_absolute()) throw std::runtime_error("launcher path");
        const auto directory = exe.parent_path();
        const auto home = directory.parent_path().parent_path();
        if (!SetEnvironmentVariableW(L"JINRA_RECOMP_HOME", home.c_str())) throw std::runtime_error("home");
        std::wstring command = quote(exe.wstring()) + L" --play-installed " + quote(game.wstring());
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            directory.c_str(), &startup, &process)) throw std::runtime_error("launch");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    } catch (...) {
        MessageBoxW(nullptr, L"N\u00e3o foi poss\u00edvel abrir o launcher do Jinra-Recomp. Abre novamente o setup para recuperar o launcher.\n\n"
                         L"Could not open the Jinra-Recomp launcher. Open setup again to restore it.",
                    L"Jinra-Ocarina", MB_OK | MB_ICONERROR);
        return 1;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr wchar_t PayloadId[] = L"0.1.6-4aee236";
constexpr char Trailer[] = "JINRAPK1";

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

bool unpack_payload(const fs::path& executable, const fs::path& path) {
    std::ifstream input(executable, std::ios::binary);
    input.seekg(-16, std::ios::end);
    const auto trailer_offset = input.tellg();
    uint64_t length = 0;
    char magic[8]{};
    input.read(reinterpret_cast<char*>(&length), sizeof(length));
    input.read(magic, sizeof(magic));
    if (!input || std::string_view(magic, 8) != Trailer || !length ||
        length > static_cast<uint64_t>(trailer_offset)) return false;
    input.seekg(static_cast<std::streamoff>(trailer_offset) - static_cast<std::streamoff>(length));
    std::ofstream output(path, std::ios::binary);
    std::vector<char> buffer(1 << 20);
    while (length) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(length, buffer.size()));
        if (!input.read(buffer.data(), count) || !output.write(buffer.data(), count)) return false;
        length -= count;
    }
    output.close();
    return output.good();
}

DWORD run(const fs::path& executable, std::wstring command, const fs::path& folder, bool wait) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process)) return 1;
    CloseHandle(process.hThread);
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        if (!GetExitCodeProcess(process.hProcess, &code)) code = 1;
    }
    CloseHandle(process.hProcess);
    return code;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    try {
        wchar_t module[32768], local[32768], system[32768];
        auto count = GetModuleFileNameW(nullptr, module, 32768);
        if (count == 0 || count >= 32768) throw std::runtime_error("path");
        DWORD size = GetEnvironmentVariableW(L"JINRA_RECOMP_HOME", local, 32768);
        const bool override_home = size > 0 && size < 32768;
        if (!override_home) size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!size || size >= 32768) throw std::runtime_error("home");
        fs::path base = local;
        if (!override_home) base /= L"Jinra-Recomp";
        const auto root = fs::absolute(base / L"launcher" / PayloadId);
        const auto app = root / L"Jinra-Recomp.UI.exe";
        if (!fs::is_regular_file(app) || !fs::is_regular_file(root / L"kit.zip")) {
            fs::create_directories(root.parent_path());
            const auto staging = fs::path(root.wstring() + L".partial-" + std::to_wstring(GetCurrentProcessId()));
            if (!fs::create_directory(staging)) throw std::runtime_error("stage");
            const auto zip = staging / L"launcher.zip";
            try {
                if (!unpack_payload(module, zip)) throw std::runtime_error("package");
                if (!GetSystemDirectoryW(system, 32768)) throw std::runtime_error("system");
                const auto tar = fs::path(system) / L"tar.exe";
                const auto args = quote(tar.wstring()) + L" -xf " + quote(zip.wstring()) + L" -C " + quote(staging.wstring());
                if (run(tar, args, staging, true) != 0 || !fs::is_regular_file(staging / app.filename()) ||
                    !fs::is_regular_file(staging / L"kit.zip")) throw std::runtime_error("extract");
                fs::remove(zip);
                std::error_code error;
                fs::rename(staging, root, error);
                if (error && (!fs::is_regular_file(app) || !fs::is_regular_file(root / L"kit.zip"))) {
                    // A previous installation may have been interrupted or had a file removed.
                    fs::remove_all(root);
                    fs::rename(staging, root);
                }
            } catch (...) {
                std::error_code ignored;
                fs::remove_all(staging, ignored);
                throw;
            }
            std::error_code ignored;
            fs::remove_all(staging, ignored);
        }
        const std::wstring tail(arguments ? arguments : L"");
        return static_cast<int>(run(app, quote(app.wstring()) + L" " + tail, root, tail.starts_with(L"--")));
    } catch (...) {
        MessageBoxW(nullptr, L"N\u00e3o foi poss\u00edvel abrir o pacote do Jinra-Recomp. Verifica o espa\u00e7o livre e volta a abrir o execut\u00e1vel completo.",
            L"Jinra-Recomp", MB_OK | MB_ICONERROR);
        return 1;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include "nx_disk_source.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;

constexpr uint64_t ZeldaTitle = 0x0400486024dec000;
constexpr uint32_t ZeldaUpdate = 131072;
constexpr std::array<uint8_t, 16> ZeldaBuild = {
    0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae,
    0x4e, 0xa0, 0x1e, 0xb5, 0xae, 0xb1, 0x3b, 0x1a
};

bool decode(void* context, const uint8_t* frame, size_t length, uint8_t* out,
            size_t capacity, size_t* written, size_t* consumed) {
    ZSTD_frameHeader header{};
    if (ZSTD_getFrameHeader(&header, frame, length) != 0 || header.frameType != ZSTD_frame ||
        header.dictID != 0 || header.windowSize > (1u << 20) ||
        ZSTD_findFrameCompressedSize(frame, length) != length) {
        return false;
    }
    *written = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(context), out, capacity, frame, length);
    *consumed = length;
    return !ZSTD_isError(*written);
}

template<class T> T require(nx::Result<T> result) {
    if (!result) throw std::runtime_error(result.error.message);
    return std::move(*result.value);
}

using Files = std::map<fs::path, nx::FileInfo>;

fs::path member_path(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("/\\:<>\"|?*") != std::string_view::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; })) {
        throw std::runtime_error("Nome de ficheiro invalido no arquivo NX");
    }
    const auto* utf8 = reinterpret_cast<const char8_t*>(name.data());
    const fs::path path(std::u8string_view(utf8, name.size()));
    auto stem = path.wstring();
    if (const auto dot = stem.find(L'.'); dot != std::wstring::npos) stem.resize(dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
         std::wstring_view(L"0123456789¹²³").find(stem.back()) != std::wstring_view::npos)) {
        throw std::runtime_error("Nome reservado no arquivo NX");
    }
    return path;
}

void collect(nx::Archive& archive, nx::DirectoryHandle directory, const fs::path& parent,
             Files& files, unsigned depth = 0) {
    if (depth > 64) throw std::runtime_error("Pastas demasiado profundas no arquivo NX");
    require(archive.enumerate(directory, [&](const nx::DirectoryEntryView& entry) {
        auto path = parent / member_path(entry.name);
        if (entry.type == nx::EntryType::directory) {
            collect(archive, entry.directory, path, files, depth + 1);
        } else {
            files.insert_or_assign(std::move(path), nx::FileInfo{entry.file, entry.type, entry.size});
        }
        return true;
    }));
}

Files inventory(nx::Archive& archive) {
    const auto& snapshots = archive.snapshots();
    const auto& programs = archive.programs();
    const auto& applications = archive.applications();
    std::optional<uint32_t> update;
    for (uint32_t index = 0; index < snapshots.size(); ++index) {
        const auto& snapshot = snapshots[index];
        if (snapshot.program_index >= programs.size()) continue;
        const auto& program = programs[snapshot.program_index];
        if (program.id == ZeldaTitle && applications.at(program.application_index).id == ZeldaTitle &&
            snapshot.type <= 1 && std::equal(ZeldaBuild.begin(), ZeldaBuild.end(), snapshot.build_id.begin())) {
            if (snapshot.type == 0 || snapshot.numeric_version == ZeldaUpdate) update = index;
        }
    }
    if (!update) throw std::runtime_error("Escolhe o Ocarina of Time da Switch 2 na versao 1.1.1 (e0f3ccfa).");

    Files files;
    auto add = [&](uint32_t index) {
        auto root = require(archive.root_directory(index));
        for (const char* name : {"exefs", "romfs"}) {
            auto entry = archive.lookup(root, name);
            if (entry && entry.value->type == nx::EntryType::directory)
                collect(archive, entry.value->directory, name, files);
        }
    };
    const auto& chosen = snapshots[*update];
    if (chosen.type == 1) {
        require(archive.dependencies(*update, [&](const nx::Dependency& dependency) {
            for (uint32_t j = 0; j < snapshots.size(); ++j) {
                if (snapshots[j].type == 0 && snapshots[j].identity == dependency.target_identity &&
                    snapshots[j].program_index == chosen.program_index) add(j);
            }
            return true;
        }));
    }
    add(*update);

    const auto exefs_end = files.lower_bound(fs::path("romfs"));
    for (auto it = files.begin(); it != exefs_end; ++it) {
        if (!it->first.has_parent_path() || it->first.parent_path() != fs::path("exefs"))
            throw std::runtime_error("O arquivo NX tem um modulo ExeFS fora da pasta esperada.");
    }
    for (const auto* module : {"main", "main.npdm", "rtld", "sdk", "subsdk0"}) {
        if (!files.contains(fs::path("exefs") / module))
            throw std::runtime_error(std::string("Falta o modulo ") + module + " no arquivo NX.");
    }
    if (files.rbegin()->first.begin()->string() != "romfs")
        throw std::runtime_error("O arquivo NX nao contem os dados RomFS.");

    const auto main = files.at(fs::path("exefs") / "main");
    std::array<uint8_t, 0x50> header{};
    const auto read = archive.read_file(main.id, 0, header.size(), header.data(), header.size());
    if (!read || read.bytes != header.size() || std::string_view(reinterpret_cast<char*>(header.data()), 4) != "NSO0" ||
        !std::equal(ZeldaBuild.begin(), ZeldaBuild.end(), header.begin() + 0x40)) {
        throw std::runtime_error("O modulo main nao corresponde ao Zelda 1.1.1 (e0f3ccfa).");
    }
    return files;
}

void extract(nx::Archive& archive, const Files& files, const fs::path& output) {
    if (!fs::create_directory(output)) throw std::runtime_error("A pasta temporaria ja existe.");
    uint64_t done = 0, total = 0;
    for (const auto& [path, file] : files) total += file.size;
    if (fs::space(output).available < total) throw std::runtime_error("Disco sem espaco para extrair a copia do jogo.");
    std::vector<uint8_t> buffer(2 << 20);
    unsigned previous = 0;
    for (const auto& [path, file] : files) {
        const auto target = output / path;
        fs::create_directories(target.parent_path());
        if (fs::exists(target)) throw std::runtime_error("Dois ficheiros NX partilham o mesmo nome no Windows.");
        std::ofstream stream(target, std::ios::binary);
        if (!stream) throw std::runtime_error("Falha ao criar um ficheiro do jogo.");
        nx::Sha256 hash;
        for (uint64_t offset = 0; offset < file.size;) {
            const auto length = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file.size - offset));
            const auto read = archive.read_file(file.id, offset, length, buffer.data(), buffer.size());
            if (!read || read.bytes != length) throw std::runtime_error("Falha de integridade num bloco NX.");
            hash.update(buffer.data(), length);
            stream.write(reinterpret_cast<const char*>(buffer.data()), length);
            offset += length;
            done += length;
            const auto percent = static_cast<unsigned>(done * 100 / total);
            if (percent != previous) {
                std::cout << "@@progress " << percent / 100.0 << std::endl;
                previous = percent;
            }
        }
        stream.close();
        if (!stream || hash.finish() != require(archive.file_digest(file.id)))
            throw std::runtime_error("Falha ao gravar ou validar um ficheiro NX.");
    }
}

int wmain(int argc, wchar_t** argv) {
    try {
        const bool check = argc == 3 && std::wstring_view(argv[1]) == L"check";
        const bool unpack = argc == 4 && std::wstring_view(argv[1]) == L"extract";
        if (!check && !unpack) throw std::runtime_error("Uso: jinra-nx check ARQUIVO | extract ARQUIVO PASTA");
        auto source = require(nx::DiskSource::open(argv[2]));
        std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
        if (!context) throw std::runtime_error("Memoria insuficiente para abrir o arquivo NX.");
        nx::OpenOptions options;
        options.zstd = decode;
        options.zstd_context = context.get();
        auto archive = require(nx::Archive::open(*source, options));
        require(archive.mount());
        auto files = inventory(archive);
        std::cout << "@@check ok 1.1.1 e0f3ccfa (" << files.size() << " ficheiros)" << std::endl;
        if (unpack) extract(archive, files, argv[3]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "@@error " << e.what() << '\n';
        return 1;
    }
}

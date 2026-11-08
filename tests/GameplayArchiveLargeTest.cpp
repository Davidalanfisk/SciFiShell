#include "GameplayArchive.h"
#include "GameplayLimits.h"
#include "miniz.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;

size_t zeros(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
    const auto limit = opaque ? *static_cast<const std::uint64_t*>(opaque) : sf::kGameplayAssetLimit;
    if (offset >= limit) return 0;
    size = std::min<std::uint64_t>(size, limit - offset);
    std::memset(buffer, 0, size);
    return size;
}

std::uint32_t read32(const std::string& data, size_t pos) {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) result |= std::uint32_t(static_cast<unsigned char>(data[pos + i])) << (8 * i);
    return result;
}

void write32(std::string& data, size_t pos, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) data[pos + i] = static_cast<char>(value >> (8 * i));
}

int main(int argc, char** argv) {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        if (!ok) { std::cerr << text << '\n'; ++failures; }
    };
    const auto path = fs::temp_directory_path() / ("scifi-large-archive-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
    mz_zip_archive zip{};
    bool created = mz_zip_writer_init_file(&zip, path.string().c_str(), 0) &&
        mz_zip_writer_add_mem(&zip, "plugin.json", "x", 1, MZ_DEFAULT_COMPRESSION) &&
        mz_zip_writer_add_mem(&zip, "scripts/main.lua", "x", 1, MZ_DEFAULT_COMPRESSION) &&
        mz_zip_writer_add_read_buf_callback(&zip, "assets/large.glb", zeros, nullptr,
            sf::kGameplayAssetLimit, nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0) &&
        mz_zip_writer_finalize_archive(&zip);
    if (!created) std::cerr << "ZIP writer: " << mz_zip_get_error_string(mz_zip_get_last_error(&zip)) << '\n';
    mz_zip_writer_end(&zip);
    check(created, "create 100 MiB archive fixture");
    std::string error;
    size_t seen = 0;
    auto begin = [](const std::string&, const std::string&, std::string&) { return true; };
    auto asset = [&](const std::string& name, const std::string& bytes, std::string&) {
        seen = bytes.size();
        return name == "assets/large.glb" && bytes.size() == sf::kGameplayAssetLimit &&
               bytes.front() == '\0' && bytes.back() == '\0';
    };
    const bool loaded = created && sf::readGameplayArchiveFile(path, begin, asset, error);
    if (created && !loaded) std::cerr << "ZIP reader: " << error << '\n';
    check(loaded && seen == 100ull * 1024 * 1024, "file-backed archive accepts and extracts exactly 100 MiB");
    if (created) {
        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        const auto eocd = data.rfind(std::string("PK\x05\x06", 4));
        if (eocd != std::string::npos && eocd + 22 == data.size()) {
            const auto directory = read32(data, eocd + 16);
            const auto sparsePath = fs::path(path.string() + ".sparse");
            bool sparse = directory < eocd;
#ifdef _WIN32
            HANDLE sparseFile = CreateFileW(sparsePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD ignored = 0;
            sparse = sparse && sparseFile != INVALID_HANDLE_VALUE &&
                     DeviceIoControl(sparseFile, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
            if (sparseFile != INVALID_HANDLE_VALUE) CloseHandle(sparseFile);
#else
            { std::ofstream out(sparsePath, std::ios::binary); sparse = sparse && bool(out); }
#endif
            if (sparse) {
                std::string tail = data.substr(directory);
                const auto newDirectory = sf::kGameplayArchiveLimit - tail.size();
                write32(tail, eocd - directory + 16, static_cast<std::uint32_t>(newDirectory));
                std::fstream out(sparsePath, std::ios::binary | std::ios::in | std::ios::out);
                out.write(data.data(), directory);
                out.seekp(static_cast<std::streamoff>(newDirectory));
                out.write(tail.data(), static_cast<std::streamsize>(tail.size()));
                out.close();
                sparse = bool(out);
            }
            if (argc == 2 && std::string(argv[1]) == "--expanded-limit") {
                const auto expandedPath = fs::path(path.string() + ".expanded");
                mz_zip_archive writer{};
                bool ok = mz_zip_writer_init_file(&writer, expandedPath.string().c_str(), 0) &&
                    mz_zip_writer_add_mem(&writer, "plugin.json", "x", 1, MZ_DEFAULT_COMPRESSION) &&
                    mz_zip_writer_add_mem(&writer, "scripts/main.lua", "x", 1, MZ_DEFAULT_COMPRESSION);
                std::uint64_t remaining = sf::kGameplayArchiveExpandedLimit - 2;
                for (int i = 0; ok && remaining; ++i) {
                    std::uint64_t amount = std::min<std::uint64_t>(remaining, sf::kGameplayAssetLimit);
                    const auto name = "assets/" + std::to_string(i) + ".glb";
                    ok = mz_zip_writer_add_read_buf_callback(&writer, name.c_str(), zeros, &amount,
                        amount, nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0);
                    remaining -= amount;
                }
                ok = ok && mz_zip_writer_finalize_archive(&writer);
                if (!ok) std::cerr << "Expanded ZIP writer: " << mz_zip_get_error_string(mz_zip_get_last_error(&writer)) << '\n';
                mz_zip_writer_end(&writer);
                std::uint64_t expanded = 2;
                bool began = false;
                auto countBegin = [&](const std::string&, const std::string&, std::string&) { began = true; return true; };
                auto countAsset = [&](const std::string&, const std::string& bytes, std::string&) {
                    expanded += bytes.size(); return true;
                };
                check(ok && sf::readGameplayArchiveFile(expandedPath, countBegin, countAsset, error) &&
                      expanded == sf::kGameplayArchiveExpandedLimit, "accept exactly 1 GiB expanded entries");
                if (ok) {
                    std::ifstream in(expandedPath, std::ios::binary);
                    std::string data((std::istreambuf_iterator<char>(in)), {});
                    in.close();
                    const auto name = data.rfind("assets/10.glb");
                    const auto central = name == std::string::npos ? std::string::npos : data.rfind(std::string("PK\x01\x02", 4), name);
                    check(central != std::string::npos && name == central + 46, "find final asset central directory");
                    if (central != std::string::npos && name == central + 46) {
                        write32(data, central + 24, read32(data, central + 24) + 1);
                        { std::ofstream out(expandedPath, std::ios::binary | std::ios::trunc);
                          out.write(data.data(), static_cast<std::streamsize>(data.size())); }
                        began = false;
                        check(!sf::readGameplayArchiveFile(expandedPath, countBegin, countAsset, error) && !began,
                              "reject expanded entries one byte over limit before extraction");
                    }
                }
                std::error_code cleanup;
                fs::remove(expandedPath, cleanup);
                check(!cleanup, "remove expanded archive fixture");
            }
            seen = 0;
            check(sparse && fs::file_size(sparsePath) == sf::kGameplayArchiveLimit &&
                  sf::readGameplayArchiveFile(sparsePath, begin, asset, error),
                  "file-backed archive accepts exactly 1 GiB on disk");
            if (sparse) {
                fs::resize_file(sparsePath, sf::kGameplayArchiveLimit + 1);
                check(!sf::readGameplayArchiveFile(sparsePath, begin, asset, error) &&
                      error.find("too large") != std::string::npos,
                      "file-backed archive rejects ZIP one byte over disk limit");
            }
            std::error_code cleanup;
            fs::remove(sparsePath, cleanup);
            check(!cleanup, "remove sparse archive fixture");
        } else {
            check(false, "find archive end record");
        }
        const auto name = data.rfind("assets/large.glb");
        const auto central = name == std::string::npos ? std::string::npos : data.rfind(std::string("PK\x01\x02", 4), name);
        check(central != std::string::npos && name == central + 46, "find asset central directory");
        if (central != std::string::npos && name == central + 46) {
            const auto tooLarge = static_cast<std::uint32_t>(sf::kGameplayAssetLimit + 1);
            for (int i = 0; i < 4; ++i) data[central + 24 + i] = static_cast<char>(tooLarge >> (8 * i));
            in.close();
            { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(data.data(), data.size()); }
            seen = 0;
            check(!sf::readGameplayArchiveFile(path, begin, asset, error) && seen == 0,
                  "file-backed archive rejects asset one byte over limit before extraction");
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
    check(!ec, "remove large archive fixture");
    return failures ? 1 : 0;
}

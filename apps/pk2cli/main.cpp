#include "pk2/archive.h"
#include "pk2/crypto.h"
#include "pk2/path.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string password{std::string(pk2::kOfficialSroPassword)};
    bool recurse{false};
    bool overwrite{false};
};

void usage() {
    std::cout
        << "PK2 Workbench PRO CLI by kahme247\n\n"
        << "Usage:\n"
        << "  pk2cli md5 <text>\n"
        << "  pk2cli create <out.pk2> [--password=169841]\n"
        << "  pk2cli diagnose <archive.pk2> [--password=169841]\n"
        << "  pk2cli list <archive.pk2> [--password=169841]\n"
        << "  pk2cli extract <archive.pk2> <archive-path> <out-dir> [--children] [--overwrite] [--password=169841]\n"
        << "  pk2cli extract-all <archive.pk2> <out-dir> [--overwrite] [--password=169841]\n"
        << "  pk2cli import-file <archive.pk2> <source-file> <archive-path> <out.pk2> [--password=169841]\n"
        << "  pk2cli import-folder <archive.pk2> <source-dir> <archive-path> <out.pk2> [--password=169841]\n"
        << "  pk2cli delete <archive.pk2> <archive-path> <out.pk2> [--password=169841]\n"
        << "  pk2cli defragment <archive.pk2> [out.pk2] [--password=169841]\n";
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

void printBlockGuess(const std::string& label, const std::vector<std::uint8_t>& bytes) {
    std::cout << label << '\n';
    for (int slot = 0; slot < 5; ++slot) {
        const auto base = static_cast<std::size_t>(slot * 128);
        const auto type = bytes[base];
        std::string name;
        for (std::size_t i = 0; i < 81; ++i) {
            const auto ch = bytes[base + 1 + i];
            if (ch == 0) {
                break;
            }
            if (ch < 32 || ch > 126) {
                name.push_back('.');
            } else {
                name.push_back(static_cast<char>(ch));
            }
        }
        std::cout << "  slot " << slot
                  << " type=" << static_cast<int>(type)
                  << " name=" << name
                  << " pos=" << readLe64(bytes.data() + base + 106)
                  << " size=" << readLe32(bytes.data() + base + 114)
                  << " next=" << readLe64(bytes.data() + base + 118)
                  << '\n';
    }
}

int scoreBlockGuess(const std::vector<std::uint8_t>& bytes, std::uint64_t archiveSize) {
    int score = 0;
    for (int slot = 0; slot < 20; ++slot) {
        const auto base = static_cast<std::size_t>(slot * 128);
        const auto type = bytes[base];
        if (type == 0) {
            continue;
        }
        if (type != 1 && type != 2) {
            return -1;
        }

        std::string name;
        for (std::size_t i = 0; i < 81; ++i) {
            const auto ch = bytes[base + 1 + i];
            if (ch == 0) {
                break;
            }
            if (ch < 32 || ch > 126 || ch == '/' || ch == '\\') {
                return -1;
            }
            name.push_back(static_cast<char>(ch));
        }
        if (name.empty()) {
            return -1;
        }
        const auto position = readLe64(bytes.data() + base + 106);
        const auto size = readLe32(bytes.data() + base + 114);
        const auto next = readLe64(bytes.data() + base + 118);
        if (position != 0 && position >= archiveSize) {
            return -1;
        }
        if (next != 0 && next >= archiveSize) {
            return -1;
        }
        if (type == 2 && position + size > archiveSize) {
            return -1;
        }
        score += 10 + static_cast<int>(name.size());
    }
    return score;
}

std::vector<std::uint8_t> asciiBytes(const std::string& value) {
    return {value.begin(), value.end()};
}

std::vector<std::uint8_t> headerSlice(const std::vector<std::uint8_t>& header,
                                      std::size_t offset,
                                      std::size_t size) {
    if (offset + size > header.size()) {
        return {};
    }
    return {header.begin() + static_cast<std::ptrdiff_t>(offset),
            header.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

void tryCandidateKey(const std::string& label,
                     const std::vector<std::uint8_t>& key,
                     const std::vector<std::uint8_t>& block,
                     std::uint64_t archiveSize,
                     pk2::KeyScheduleMode keyMode = pk2::KeyScheduleMode::Standard) {
    if (key.empty()) {
        return;
    }
    pk2::Blowfish cipher(key.data(), key.size(), keyMode);
    for (const auto endian : {pk2::BlockEndian::Big, pk2::BlockEndian::Little}) {
        auto decoded = block;
        cipher.decryptBuffer(decoded, endian);
        const auto score = scoreBlockGuess(decoded, archiveSize);
        if (score > 0) {
            printBlockGuess(label + (endian == pk2::BlockEndian::Big ? " big" : " little") +
                                " score=" + std::to_string(score),
                            decoded);
        }
    }
}

Options parseOptions(const std::vector<std::string>& args) {
    Options options;
    for (const auto& arg : args) {
        if (arg.rfind("--password=", 0) == 0) {
            options.password = arg.substr(std::string("--password=").size());
        } else if (arg == "--children") {
            options.recurse = true;
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        }
    }
    return options;
}

std::vector<std::string> positional(const std::vector<std::string>& args) {
    std::vector<std::string> result;
    for (const auto& arg : args) {
        if (arg.rfind("--", 0) != 0) {
            result.push_back(arg);
        }
    }
    return result;
}

void printEntry(const pk2::EntryInfo& entry) {
    std::cout << (entry.type == pk2::EntryType::Folder ? "dir  " : "file ")
              << entry.path;
    if (entry.type == pk2::EntryType::File) {
        std::cout << " (" << entry.size << " bytes)";
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 1;
        }

        std::vector<std::string> rawArgs;
        for (int i = 2; i < argc; ++i) {
            rawArgs.emplace_back(argv[i]);
        }
        const auto options = parseOptions(rawArgs);
        const auto args = positional(rawArgs);
        const std::string command = argv[1];

        if (command == "md5") {
            if (args.size() != 1) {
                usage();
                return 1;
            }
            std::cout << pk2::md5Hex(args[0]) << '\n';
            return 0;
        }

        if (command == "create") {
            if (args.size() != 1) {
                usage();
                return 1;
            }
            auto archive = pk2::Pk2Archive::createNew(options.password);
            archive.saveAs(fs::path(args[0]));
            std::cout << "Created " << args[0] << '\n';
            return 0;
        }

        if (command == "diagnose") {
            if (args.size() != 1) {
                usage();
                return 1;
            }

            std::ifstream input(args[0], std::ios::binary);
            if (!input) {
                throw pk2::Pk2Error("Could not open archive.");
            }
            const auto archiveSize = fs::file_size(args[0]);
            std::vector<std::uint8_t> header(256);
            std::vector<std::uint8_t> block(2560);
            input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
            input.seekg(256, std::ios::beg);
            input.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));

            std::cout << "Header first 42 bytes:\n";
            for (std::size_t i = 0; i < 42; ++i) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(header[i]) << ((i % 16 == 15) ? '\n' : ' ');
            }
            std::cout << std::dec << "\n";

            printBlockGuess("plain", block);
            if (!options.password.empty()) {
                pk2::Blowfish direct(options.password);
                auto big = block;
                direct.decryptBuffer(big, pk2::BlockEndian::Big);
                printBlockGuess("blowfish direct big-endian", big);

                auto little = block;
                direct.decryptBuffer(little, pk2::BlockEndian::Little);
                printBlockGuess("blowfish direct little-endian", little);

                pk2::Blowfish joymax(options.password, pk2::KeyScheduleMode::JoymaxCompatible);
                auto joymaxLittle = block;
                joymax.decryptBuffer(joymaxLittle, pk2::BlockEndian::Little);
                printBlockGuess("blowfish joymax-compatible little-endian", joymaxLittle);

                const auto md5 = pk2::md5Hex(options.password);
                pk2::Blowfish md5Cipher(md5);
                auto md5Big = block;
                md5Cipher.decryptBuffer(md5Big, pk2::BlockEndian::Big);
                printBlockGuess("blowfish md5(password) big-endian", md5Big);
            }

            std::cout << "\nscored candidate keys:\n";
            std::vector<std::pair<std::string, std::vector<std::uint8_t>>> candidates;
            candidates.push_back({"password ascii", asciiBytes(options.password)});
            candidates.push_back({"md5(password) ascii", asciiBytes(pk2::md5Hex(options.password))});
            candidates.push_back({"SILKROADVERSION", asciiBytes("SILKROADVERSION")});
            candidates.push_back({"SILKROAD", asciiBytes("SILKROAD")});
            candidates.push_back({"JoyMax File Manager!", asciiBytes("JoyMax File Manager!")});
            for (std::size_t off = 0; off < 64; ++off) {
                for (std::size_t len : {4u, 6u, 8u, 16u, 32u}) {
                    const auto key = headerSlice(header, off, len);
                    if (!key.empty()) {
                        candidates.push_back({"header[" + std::to_string(off) + ":" +
                                                  std::to_string(off + len) + "]",
                                              key});
                    }
                }
            }
            for (const auto& candidate : candidates) {
                tryCandidateKey(candidate.first, candidate.second, block, archiveSize);
                tryCandidateKey(candidate.first + " joymax-compatible",
                                candidate.second,
                                block,
                                archiveSize,
                                pk2::KeyScheduleMode::JoymaxCompatible);
            }
            return 0;
        }

        if (command == "list") {
            if (args.size() != 1) {
                usage();
                return 1;
            }
            const auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            for (const auto& entry : archive.listTree()) {
                printEntry(entry);
            }
            return 0;
        }

        if (command == "extract") {
            if (args.size() != 3) {
                usage();
                return 1;
            }
            const auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            archive.extract(args[1], fs::path(args[2]), options.recurse,
                            options.overwrite ? pk2::OverwritePolicy::Replace
                                              : pk2::OverwritePolicy::Fail);
            std::cout << "Extracted " << args[1] << '\n';
            return 0;
        }

        if (command == "extract-all") {
            if (args.size() != 2) {
                usage();
                return 1;
            }
            const auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            archive.extract("", fs::path(args[1]), true,
                            options.overwrite ? pk2::OverwritePolicy::Replace
                                              : pk2::OverwritePolicy::Fail);
            std::cout << "Extracted all entries\n";
            return 0;
        }

        if (command == "import-file") {
            if (args.size() != 4) {
                usage();
                return 1;
            }
            auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            archive.importFile(fs::path(args[1]), args[2]);
            archive.saveAs(fs::path(args[3]));
            std::cout << "Imported file and saved " << args[3] << '\n';
            return 0;
        }

        if (command == "import-folder") {
            if (args.size() != 4) {
                usage();
                return 1;
            }
            auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            archive.importFolder(fs::path(args[1]), args[2]);
            archive.saveAs(fs::path(args[3]));
            std::cout << "Imported folder and saved " << args[3] << '\n';
            return 0;
        }

        if (command == "delete") {
            if (args.size() != 3) {
                usage();
                return 1;
            }
            auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            archive.deleteEntry(args[1]);
            archive.saveAs(fs::path(args[2]));
            std::cout << "Deleted entry and saved " << args[2] << '\n';
            return 0;
        }

        if (command == "defragment") {
            if (args.size() != 1 && args.size() != 2) {
                usage();
                return 1;
            }
            auto archive = pk2::Pk2Archive::open(fs::path(args[0]), options.password);
            if (args.size() == 2) {
                archive.saveAs(fs::path(args[1]));
                std::cout << "Defragmented and saved to " << args[1] << '\n';
            } else {
                archive.saveDefragmented();
                std::cout << "Defragmented and saved " << args[0] << '\n';
            }
            return 0;
        }

        usage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 2;
    }
}

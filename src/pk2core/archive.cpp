#include "pk2/archive.h"

#include "pk2/path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace pk2 {

enum class Pk2Archive::CryptoMode : std::uint8_t {
    Plain,
    BlowfishBigEndian,
    BlowfishLittleEndian,
    JoymaxBlowfishBigEndian,
    JoymaxBlowfishLittleEndian
};

struct Pk2Archive::Node {
    EntryType type{EntryType::Folder};
    std::string name;
    Node* parent{};
    std::vector<std::unique_ptr<Node>> children;

    std::uint64_t blockOffset{};
    std::size_t slotIndex{};
    std::uint64_t folderFirstBlockOffset{};

    std::uint64_t sourceOffset{};
    std::uint64_t size{};
    std::vector<std::uint8_t> importedData;
    bool imported{};

    bool isFolder() const { return type == EntryType::Folder; }
    bool isFile() const { return type == EntryType::File; }
};

namespace {

constexpr std::size_t kHeaderSize = 256;
constexpr std::size_t kEntrySize = 128;
constexpr std::size_t kEntriesPerBlock = 20;
constexpr std::size_t kEntryBlockSize = kEntrySize * kEntriesPerBlock;
constexpr std::size_t kEntryNameOffset = 1;
constexpr std::size_t kEntryNameSize = 81;
constexpr std::size_t kEntryPositionOffset = 106;
constexpr std::size_t kEntrySizeOffset = 114;
constexpr std::size_t kEntryNextBlockOffset = 118;
constexpr std::string_view kJoymaxMagic = "JoyMax File Manager!";

struct DiskEntry {
    std::uint8_t type{};
    std::string name;
    std::uint64_t position{};
    std::uint32_t size{};
    std::uint64_t nextBlock{};
};

std::string filesystemErrorText(const std::string& action,
                                const fs::path& path,
                                const std::error_code& error) {
    return action + ": " + pathUtf8(path) + " (" + error.message() + ")";
}

std::uint64_t fileSizeChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText(action, path, error));
    }
    return size;
}

bool existsChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto exists = fs::exists(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText(action, path, error));
    }
    return exists;
}

bool isRegularFileChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isRegular = fs::is_regular_file(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText(action, path, error));
    }
    return isRegular;
}

bool isDirectoryChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isDirectory = fs::is_directory(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText(action, path, error));
    }
    return isDirectory;
}

void createDirectoriesChecked(const fs::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    fs::create_directories(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText("Could not create directory", path, error));
    }
}

void copyFileChecked(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
    if (error) {
        throw Pk2Error("Could not copy file from " + pathUtf8(source) + " to " +
                       pathUtf8(destination) + " (" + error.message() + ")");
    }
}

void removeChecked(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
    if (error) {
        throw Pk2Error(filesystemErrorText("Could not remove file", path, error));
    }
}

void renameChecked(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    fs::rename(source, destination, error);
    if (error) {
        throw Pk2Error("Could not rename file from " + pathUtf8(source) + " to " +
                       pathUtf8(destination) + " (" + error.message() + ")");
    }
}

std::uint32_t readU32Le(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t readU64Le(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

void writeU32Le(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value & 0xff);
    bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void writeU64Le(std::uint8_t* bytes, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
    }
}

std::vector<std::uint8_t> readAllBytes(const fs::path& path,
                                       std::uint64_t& completed,
                                       std::uint64_t total,
                                       std::string_view progressPath,
                                       const ProgressCallback& progress) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Pk2Error("Could not open file: " + pathUtf8(path));
    }

    const auto size = fileSizeChecked(path, "Could not read file size");
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw Pk2Error("Input file is too large to import into memory: " + pathUtf8(path));
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::uint64_t offset = 0;
    while (offset < size) {
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(size - offset, 1024 * 1024));
        input.read(reinterpret_cast<char*>(bytes.data() + static_cast<std::size_t>(offset)),
                   static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw Pk2Error("Could not read all bytes from file: " + pathUtf8(path));
        }
        offset += chunk;
        completed += chunk;
        if (progress) {
            progress(completed, total, progressPath);
        }
    }

    if (size == 0 && progress) {
        progress(completed, total, progressPath);
    }
    return bytes;
}

class ArchiveReader {
public:
    explicit ArchiveReader(fs::path path)
        : path_(std::move(path)),
          input_(path_, std::ios::binary),
          size_(fileSizeChecked(path_, "Could not read archive size")) {
        if (!input_) {
            throw Pk2Error("Could not open file: " + pathUtf8(path_));
        }
    }

    std::uint64_t size() const {
        return size_;
    }

    std::vector<std::uint8_t> readAt(std::uint64_t offset, std::size_t count) {
        if (offset > size_ || size_ - offset < count) {
            throw Pk2Error("PK2 read offset is outside the file.");
        }

        std::vector<std::uint8_t> bytes(count);
        input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
        if (input_.gcount() != static_cast<std::streamsize>(count)) {
            throw Pk2Error("Could not read all requested PK2 bytes.");
        }
        return bytes;
    }

private:
    fs::path path_;
    std::ifstream input_;
    std::uint64_t size_{};
};

std::string nodePath(const Pk2Archive::Node& node) {
    if (node.parent == nullptr) {
        return {};
    }
    std::vector<std::string> parts;
    const auto* current = &node;
    while (current != nullptr && current->parent != nullptr) {
        parts.push_back(current->name);
        current = current->parent;
    }
    std::reverse(parts.begin(), parts.end());

    std::string path;
    for (const auto& part : parts) {
        path = joinArchivePath(path, part);
    }
    return path;
}

bool isValidEntryName(const std::string& name) {
    if (name.empty() || name.size() > 80) {
        return false;
    }
    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || ch == '/' || ch == '\\' || ch == ':' ||
            ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|') {
            return false;
        }
    }
    return true;
}

bool isReservedEntryName(const std::string& name) {
    return name == "." || name == "..";
}

std::string readEntryName(const std::vector<std::uint8_t>& block, std::size_t base) {
    std::string name;
    for (std::size_t i = 0; i < kEntryNameSize; ++i) {
        const auto byte = block[base + kEntryNameOffset + i];
        if (byte == 0) {
            break;
        }
        name.push_back(static_cast<char>(byte));
    }
    return name;
}

DiskEntry readDiskEntry(const std::vector<std::uint8_t>& block, std::size_t slot) {
    const auto base = slot * kEntrySize;
    DiskEntry entry;
    entry.type = block[base];
    entry.name = readEntryName(block, base);
    entry.position = readU64Le(block.data() + base + kEntryPositionOffset);
    entry.size = readU32Le(block.data() + base + kEntrySizeOffset);
    entry.nextBlock = readU64Le(block.data() + base + kEntryNextBlockOffset);
    return entry;
}

bool rootBlockHasSelfEntry(const std::vector<std::uint8_t>& decodedRoot) {
    if (decodedRoot.size() != kEntryBlockSize) {
        return false;
    }
    for (std::size_t i = 0; i < kEntriesPerBlock; ++i) {
        const auto entry = readDiskEntry(decodedRoot, i);
        if (entry.type == 1 && entry.name == ".") {
            return true;
        }
    }
    return false;
}

int scoreEntryBlock(const std::vector<std::uint8_t>& block, std::uint64_t archiveSize) {
    if (block.size() != kEntryBlockSize) {
        return -1;
    }

    int score = 0;
    for (std::size_t i = 0; i < kEntriesPerBlock; ++i) {
        const auto entry = readDiskEntry(block, i);
        if (entry.type == 0) {
            continue;
        }
        if (entry.type != 1 && entry.type != 2) {
            return -1;
        }
        if (!isValidEntryName(entry.name)) {
            return -1;
        }
        if (entry.type == 1 && entry.size != 0) {
            return -1;
        }
        if (entry.position > 0 && entry.position >= archiveSize) {
            return -1;
        }
        if (entry.nextBlock > 0 && entry.nextBlock >= archiveSize) {
            return -1;
        }
        if (entry.type == 2 && entry.position + entry.size > archiveSize) {
            return -1;
        }
        score += 10 + static_cast<int>(entry.name.size());
    }
    return score;
}

std::vector<std::uint8_t> readRawBlock(ArchiveReader& reader, std::uint64_t offset) {
    if (offset > reader.size() || reader.size() - offset < kEntryBlockSize) {
        throw Pk2Error("Entry block offset is outside the PK2 file.");
    }
    return reader.readAt(offset, kEntryBlockSize);
}

bool isEncryptedMode(Pk2Archive::CryptoMode mode) {
    return mode != Pk2Archive::CryptoMode::Plain;
}

BlockEndian endianForMode(Pk2Archive::CryptoMode mode) {
    switch (mode) {
    case Pk2Archive::CryptoMode::BlowfishLittleEndian:
    case Pk2Archive::CryptoMode::JoymaxBlowfishLittleEndian:
        return BlockEndian::Little;
    case Pk2Archive::CryptoMode::Plain:
    case Pk2Archive::CryptoMode::BlowfishBigEndian:
    case Pk2Archive::CryptoMode::JoymaxBlowfishBigEndian:
        return BlockEndian::Big;
    }
    return BlockEndian::Big;
}

KeyScheduleMode keyScheduleForMode(Pk2Archive::CryptoMode mode) {
    switch (mode) {
    case Pk2Archive::CryptoMode::JoymaxBlowfishBigEndian:
    case Pk2Archive::CryptoMode::JoymaxBlowfishLittleEndian:
        return KeyScheduleMode::JoymaxCompatible;
    case Pk2Archive::CryptoMode::Plain:
    case Pk2Archive::CryptoMode::BlowfishBigEndian:
    case Pk2Archive::CryptoMode::BlowfishLittleEndian:
        return KeyScheduleMode::Standard;
    }
    return KeyScheduleMode::Standard;
}

std::unique_ptr<Blowfish> makeCipher(const std::string& password,
                                     Pk2Archive::CryptoMode mode) {
    if (!isEncryptedMode(mode)) {
        return nullptr;
    }
    if (password.empty()) {
        throw Pk2Error("A Blowfish password is required to decode this PK2.");
    }
    return std::make_unique<Blowfish>(password, keyScheduleForMode(mode));
}

std::vector<std::uint8_t> decodeWithMode(std::vector<std::uint8_t> block,
                                         Pk2Archive::CryptoMode mode,
                                         const Blowfish* cipher) {
    if (mode == Pk2Archive::CryptoMode::Plain) {
        return block;
    }
    if (cipher == nullptr) {
        throw Pk2Error("A Blowfish password is required to decode this PK2.");
    }
    cipher->decryptBuffer(block, endianForMode(mode));
    return block;
}

struct DecodedBlock {
    std::vector<std::uint8_t> bytes;
    Pk2Archive::CryptoMode mode{Pk2Archive::CryptoMode::Plain};
    int score{};
};

DecodedBlock chooseDecodedBlock(const std::vector<std::uint8_t>& raw,
                                const std::string& password,
                                std::uint64_t archiveSize) {
    std::vector<DecodedBlock> candidates;

    auto plain = raw;
    candidates.push_back({plain, Pk2Archive::CryptoMode::Plain,
                          scoreEntryBlock(plain, archiveSize)});

    if (!password.empty()) {
        for (const auto mode : {Pk2Archive::CryptoMode::JoymaxBlowfishLittleEndian,
                               Pk2Archive::CryptoMode::JoymaxBlowfishBigEndian,
                               Pk2Archive::CryptoMode::BlowfishLittleEndian,
                               Pk2Archive::CryptoMode::BlowfishBigEndian}) {
            auto cipher = makeCipher(password, mode);
            auto decoded = decodeWithMode(raw, mode, cipher.get());
            candidates.push_back({decoded, mode, scoreEntryBlock(decoded, archiveSize)});
        }
    }

    auto best = std::max_element(candidates.begin(), candidates.end(),
                                 [](const auto& lhs, const auto& rhs) {
                                     return lhs.score < rhs.score;
                                 });
    if (best == candidates.end() || best->score <= 0) {
        throw Pk2Error("Could not decode a valid PK2 entry block. The password may be wrong.");
    }
    return *best;
}

void addRootOffsetCandidate(std::vector<std::uint64_t>& candidates,
                            std::uint64_t value,
                            std::uint64_t archiveSize) {
    if (value >= kHeaderSize && value + kEntryBlockSize <= archiveSize &&
        std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
        candidates.push_back(value);
    }
}

std::vector<std::uint64_t> rootOffsetCandidates(const std::vector<std::uint8_t>& header,
                                                std::uint64_t archiveSize) {
    std::vector<std::uint64_t> candidates;
    addRootOffsetCandidate(candidates, kHeaderSize, archiveSize);
    const auto scanLimit = std::min<std::size_t>(128, header.size());
    for (std::size_t offset = 24; offset + 8 <= scanLimit; offset += 2) {
        addRootOffsetCandidate(candidates, readU32Le(header.data() + offset), archiveSize);
        addRootOffsetCandidate(candidates, readU64Le(header.data() + offset), archiveSize);
    }
    return candidates;
}

bool equalsCaseInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Pk2Archive::Node* childByName(Pk2Archive::Node& folder, const std::string& name) {
    for (auto& child : folder.children) {
        if (equalsCaseInsensitive(child->name, name)) {
            return child.get();
        }
    }
    return nullptr;
}

void appendFileBytes(std::ofstream& output,
                     const fs::path& sourceArchive,
                     const Pk2Archive::Node& node) {
    if (node.imported) {
        output.write(reinterpret_cast<const char*>(node.importedData.data()),
                     static_cast<std::streamsize>(node.importedData.size()));
        return;
    }

    std::ifstream input(sourceArchive, std::ios::binary);
    if (!input) {
        throw Pk2Error("Could not reopen source archive while saving.");
    }
    input.seekg(static_cast<std::streamoff>(node.sourceOffset), std::ios::beg);

    std::array<char, 64 * 1024> buffer{};
    std::uint64_t remaining = node.size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw Pk2Error("Could not copy all source file data while saving.");
        }
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
    }
}

void copyExtractedFileBytes(std::ofstream& output,
                            std::ifstream* sourceInput,
                            const Pk2Archive::Node& node,
                            std::string_view progressPath,
                            const std::function<void(std::uint64_t, std::string_view)>& progress) {
    std::vector<char> buffer(1024 * 1024);

    if (node.imported) {
        std::uint64_t offset = 0;
        while (offset < node.importedData.size()) {
            const auto chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(node.importedData.size() - offset, buffer.size()));
            output.write(reinterpret_cast<const char*>(
                             node.importedData.data() + static_cast<std::size_t>(offset)),
                         static_cast<std::streamsize>(chunk));
            offset += chunk;
            if (progress) {
                progress(chunk, progressPath);
            }
        }
        if (node.importedData.empty() && progress) {
            progress(0, progressPath);
        }
        return;
    }

    if (sourceInput == nullptr || !*sourceInput) {
        throw Pk2Error("Could not reopen source archive while extracting.");
    }
    sourceInput->clear();
    sourceInput->seekg(static_cast<std::streamoff>(node.sourceOffset), std::ios::beg);

    std::uint64_t remaining = node.size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        sourceInput->read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (sourceInput->gcount() != static_cast<std::streamsize>(chunk)) {
            throw Pk2Error("Could not copy all source file data while extracting.");
        }
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
        if (progress) {
            progress(chunk, progressPath);
        }
    }
    if (node.size == 0 && progress) {
        progress(0, progressPath);
    }
}

fs::path archivePathToFilesystem(const fs::path& base, const std::string& archivePath) {
    fs::path result = base;
    std::stringstream stream(normalizeArchivePath(archivePath));
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (!part.empty()) {
            result /= archivePathPartToFilesystem(part);
        }
    }
    return result;
}

struct ExtractWorkItem {
    const Pk2Archive::Node* node{};
    std::string archivePath;
    fs::path outputPath;
};

std::size_t extractionWorkerCount(std::size_t fileCount) {
    if (fileCount <= 1) {
        return 1;
    }

    auto cpuCount = std::thread::hardware_concurrency();
    if (cpuCount == 0) {
        cpuCount = 4;
    }
    const auto capped = std::min<unsigned int>(cpuCount, 4);
    return std::min<std::size_t>(fileCount, std::max<unsigned int>(1, capped));
}

} // namespace

struct ArchiveIo {
    static void parseBlock(Pk2Archive& archive,
                           ArchiveReader& reader,
                           const Blowfish* cipher,
                           std::uint64_t blockOffset,
                           Pk2Archive::Node& parent,
                           std::set<std::uint64_t>& visited) {
        if (blockOffset == 0 || !visited.insert(blockOffset).second) {
            return;
        }

        if (&parent == archive.root_.get()) {
            parent.folderFirstBlockOffset = blockOffset;
        }

        const auto raw = readRawBlock(reader, blockOffset);
        const auto decoded = decodeWithMode(raw, archive.cryptoMode_, cipher);
        if (scoreEntryBlock(decoded, reader.size()) < 0) {
            throw Pk2Error("A PK2 entry block could not be decoded consistently.");
        }

        std::vector<std::uint64_t> chainedBlocks;
        for (std::size_t i = 0; i < kEntriesPerBlock; ++i) {
            const auto entry = readDiskEntry(decoded, i);
            if (entry.type == 0) {
                continue;
            }
            if (entry.nextBlock != 0 && (i == kEntriesPerBlock - 1 || chainedBlocks.empty())) {
                chainedBlocks.push_back(entry.nextBlock);
            }
            if (isReservedEntryName(entry.name)) {
                continue;
            }

            auto node = std::make_unique<Pk2Archive::Node>();
            node->type = entry.type == 1 ? EntryType::Folder : EntryType::File;
            node->name = entry.name;
            node->parent = &parent;
            node->blockOffset = blockOffset;
            node->slotIndex = i;
            node->sourceOffset = entry.position;
            if (entry.type == 1) {
                node->folderFirstBlockOffset = entry.position;
            }
            node->size = entry.size;
            auto* inserted = node.get();
            parent.children.push_back(std::move(node));

            if (entry.type == 1 && entry.position != 0) {
                parseBlock(archive, reader, cipher, entry.position, *inserted, visited);
            }
        }

        for (const auto next : chainedBlocks) {
            parseBlock(archive, reader, cipher, next, parent, visited);
        }
    }
};

Pk2Archive::Pk2Archive()
    : root_(std::make_unique<Node>()),
      password_(std::string(kOfficialSroPassword)),
      cryptoMode_(CryptoMode::JoymaxBlowfishLittleEndian) {}

Pk2Archive::Pk2Archive(std::unique_ptr<Node> root, std::string password)
    : root_(std::move(root)),
      password_(std::move(password)),
      cryptoMode_(CryptoMode::JoymaxBlowfishLittleEndian) {}

Pk2Archive::~Pk2Archive() = default;
Pk2Archive::Pk2Archive(Pk2Archive&&) noexcept = default;
Pk2Archive& Pk2Archive::operator=(Pk2Archive&&) noexcept = default;

Pk2Archive Pk2Archive::createNew(std::string password) {
    auto root = std::make_unique<Node>();
    root->type = EntryType::Folder;
    Pk2Archive archive(std::move(root), std::move(password));
    archive.cryptoMode_ = archive.password_.empty() ? CryptoMode::Plain
                                                    : CryptoMode::JoymaxBlowfishLittleEndian;
    archive.dirty_ = true;
    return archive;
}

Pk2Archive Pk2Archive::open(const fs::path& path, std::string password) {
    ArchiveReader reader(path);
    if (reader.size() < kHeaderSize + kEntryBlockSize) {
        throw Pk2Error("The file is too small to be a Joymax PK2 archive.");
    }

    const auto header = reader.readAt(0, kHeaderSize);
    const std::string headerText(header.begin(),
                                 header.begin() + static_cast<std::ptrdiff_t>(
                                     std::min<std::size_t>(64, header.size())));
    if (headerText.find(kJoymaxMagic) == std::string::npos) {
        throw Pk2Error("Invalid PK2 name. JoyMax File Manager header was not found.");
    }

    auto root = std::make_unique<Node>();
    root->type = EntryType::Folder;
    Pk2Archive archive(std::move(root), std::move(password));
    archive.sourcePath_ = path;
    archive.sourceHeader_ = header;

    std::optional<DecodedBlock> rootBlock;
    std::uint64_t rootOffset = kHeaderSize;
    try {
        rootBlock = chooseDecodedBlock(readRawBlock(reader, kHeaderSize),
                                       archive.password_,
                                       reader.size());
    } catch (const Pk2Error&) {
        for (const auto candidate : rootOffsetCandidates(header, reader.size())) {
            try {
                auto decoded = chooseDecodedBlock(readRawBlock(reader, candidate),
                                                  archive.password_,
                                                  reader.size());
                if (!rootBlock || decoded.score > rootBlock->score) {
                    rootOffset = candidate;
                    rootBlock = std::move(decoded);
                }
            } catch (const Pk2Error&) {
            }
        }
    }

    if (!rootBlock) {
        throw Pk2Error("Could not locate the root PK2 entry block.");
    }

    archive.cryptoMode_ = rootBlock->mode;
    // A valid root block holding only the "." self entry is a genuine empty
    // archive (e.g. freshly created). Anything else with no decodable
    // children is still treated as a wrong password or corrupt file.
    const bool rootSelfEntry = rootBlockHasSelfEntry(rootBlock->bytes);
    auto cipher = makeCipher(archive.password_, archive.cryptoMode_);
    std::set<std::uint64_t> visited;
    ArchiveIo::parseBlock(archive, reader, cipher.get(), rootOffset, *archive.root_, visited);
    if (archive.root_->children.empty() && !rootSelfEntry) {
        throw Pk2Error("PK2 opened, but no root entries were decoded. The password or PK2 key derivation is probably wrong.");
    }
    archive.dirty_ = false;
    return archive;
}

const fs::path& Pk2Archive::sourcePath() const {
    return sourcePath_;
}

const std::string& Pk2Archive::password() const {
    return password_;
}

bool Pk2Archive::dirty() const {
    return dirty_;
}

bool Pk2Archive::empty() const {
    return root_->children.empty();
}

std::size_t Pk2Archive::entryCount() const {
    return listTree().size();
}

EntryInfo Pk2Archive::makeInfo(const Node& node) const {
    EntryInfo info;
    info.type = node.type;
    info.path = nodePath(node);
    info.name = node.name;
    info.size = node.size;
    info.dataOffset = node.sourceOffset;
    return info;
}

std::vector<EntryInfo> Pk2Archive::listTree() const {
    std::vector<EntryInfo> entries;
    std::function<void(const Node&)> visit = [&](const Node& node) {
        if (node.parent != nullptr) {
            entries.push_back(makeInfo(node));
        }
        if (node.isFolder()) {
            for (const auto& child : node.children) {
                visit(*child);
            }
        }
    };
    visit(*root_);
    return entries;
}

std::vector<EntryInfo> Pk2Archive::children(const std::string& archiveFolderPath) const {
    const auto* folder = findNode(archiveFolderPath);
    if (folder == nullptr || !folder->isFolder()) {
        throw Pk2Error("Archive path is not a folder: " + archiveFolderPath);
    }

    std::vector<EntryInfo> result;
    for (const auto& child : folder->children) {
        result.push_back(makeInfo(*child));
    }
    return result;
}

std::optional<EntryInfo> Pk2Archive::find(const std::string& archivePath) const {
    const auto* node = findNode(archivePath);
    if (node == nullptr || node == root_.get()) {
        return std::nullopt;
    }
    return makeInfo(*node);
}

std::vector<std::uint8_t> Pk2Archive::readFile(const std::string& archivePath) const {
    const auto* node = findNode(archivePath);
    if (node == nullptr || !node->isFile()) {
        throw Pk2Error("Archive file was not found: " + archivePath);
    }
    if (node->imported) {
        return node->importedData;
    }
    if (node->size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw Pk2Error("Archive file is too large to read into memory: " + archivePath);
    }
    ArchiveReader reader(sourcePath_);
    return reader.readAt(node->sourceOffset, static_cast<std::size_t>(node->size));
}

void Pk2Archive::importFileBytes(std::vector<std::uint8_t> bytes,
                                const std::string& archivePath) {
    const auto normalized = normalizeArchivePath(archivePath);
    const auto name = archiveFileName(normalized);
    if (!isValidEntryName(name) || isReservedEntryName(name)) {
        throw Pk2Error("Invalid PK2 entry name: " + name);
    }

    auto* folder = ensureFolder(archiveParentPath(normalized));
    auto* existing = childByName(*folder, name);
    if (existing != nullptr && existing->isFolder()) {
        throw Pk2Error("A folder already exists at archive path: " + normalized);
    }
    Node* target = existing;
    if (target == nullptr) {
        auto node = std::make_unique<Node>();
        node->type = EntryType::File;
        node->name = name;
        node->parent = folder;
        target = node.get();
        folder->children.push_back(std::move(node));
    }
    target->type = EntryType::File;
    target->size = bytes.size();
    target->sourceOffset = 0;
    target->importedData = std::move(bytes);
    target->imported = true;
    dirty_ = true;
}

Pk2Archive::Node* Pk2Archive::findNode(const std::string& archivePath) {
    const auto normalized = normalizeArchivePath(archivePath);
    if (normalized.empty()) {
        return root_.get();
    }

    auto* current = root_.get();
    std::stringstream stream(normalized);
    std::string part;
    while (std::getline(stream, part, '/')) {
        current = childByName(*current, part);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

const Pk2Archive::Node* Pk2Archive::findNode(const std::string& archivePath) const {
    return const_cast<Pk2Archive*>(this)->findNode(archivePath);
}

Pk2Archive::Node* Pk2Archive::ensureFolder(const std::string& archivePath) {
    const auto normalized = normalizeArchivePath(archivePath);
    auto* current = root_.get();
    if (normalized.empty()) {
        return current;
    }

    std::stringstream stream(normalized);
    std::string part;
    while (std::getline(stream, part, '/')) {
        auto* child = childByName(*current, part);
        if (child == nullptr) {
            auto node = std::make_unique<Node>();
            node->type = EntryType::Folder;
            node->name = part;
            node->parent = current;
            child = node.get();
            current->children.push_back(std::move(node));
            structureDirty_ = true;
        }
        if (!child->isFolder()) {
            throw Pk2Error("A file already exists at archive path: " + nodePath(*child));
        }
        current = child;
    }
    return current;
}

void Pk2Archive::extract(const std::string& archivePath,
                         const fs::path& destination,
                         bool recurse,
                         OverwritePolicy overwrite) const {
    extract(archivePath, destination, recurse, overwrite, {});
}

void Pk2Archive::extract(const std::string& archivePath,
                         const fs::path& destination,
                         bool recurse,
                         OverwritePolicy overwrite,
                         ProgressCallback progress) const {
    const auto* node = findNode(archivePath);
    if (node == nullptr) {
        throw Pk2Error("Archive path was not found: " + archivePath);
    }

    std::vector<ExtractWorkItem> files;
    std::vector<fs::path> directories;
    bool needsSourceArchive = false;
    std::function<void(const Node&)> collectFiles = [&](const Node& item) {
        if (item.isFolder()) {
            if (!recurse && &item != node) {
                return;
            }
            directories.push_back(archivePathToFilesystem(destination, nodePath(item)));
            for (const auto& child : item.children) {
                collectFiles(*child);
            }
            return;
        }
        const auto currentPath = nodePath(item);
        files.push_back(ExtractWorkItem{&item,
                                        currentPath,
                                        archivePathToFilesystem(destination, currentPath)});
        needsSourceArchive = needsSourceArchive || !item.imported;
    };
    collectFiles(*node);

    std::uint64_t totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.node->size;
    }
    std::uint64_t completedBytes = 0;
    if (progress) {
        progress(completedBytes, totalBytes, archivePath);
    }

    for (const auto& directory : directories) {
        createDirectoriesChecked(directory);
    }
    for (const auto& file : files) {
        createDirectoriesChecked(file.outputPath.parent_path());
        if (existsChecked(file.outputPath, "Could not inspect output file") &&
            overwrite == OverwritePolicy::Fail) {
            throw Pk2Error("Output file already exists: " + pathUtf8(file.outputPath));
        }
    }

    const auto workerCount = extractionWorkerCount(files.size());
    auto reportSequentialProgress = [&](std::uint64_t transferred, std::string_view currentPath) {
        completedBytes += transferred;
        if (progress) {
            progress(completedBytes, totalBytes, currentPath);
        }
    };

    if (workerCount == 1) {
        std::ifstream sourceInput;
        if (needsSourceArchive) {
            sourceInput.open(sourcePath_, std::ios::binary);
            if (!sourceInput) {
                throw Pk2Error("Could not reopen source archive while extracting.");
            }
        }

        for (const auto& file : files) {
            std::ofstream output(file.outputPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw Pk2Error("Could not create output file: " + pathUtf8(file.outputPath));
            }
            copyExtractedFileBytes(output,
                                   file.node->imported ? nullptr : &sourceInput,
                                   *file.node,
                                   file.archivePath,
                                   reportSequentialProgress);
            if (!output) {
                throw Pk2Error("Could not write output file: " + pathUtf8(file.outputPath));
            }
        }
    } else {
        std::atomic<std::size_t> nextIndex{0};
        std::atomic<std::uint64_t> parallelCompleted{0};
        std::atomic<bool> failed{false};
        std::exception_ptr firstError;
        std::mutex errorMutex;
        std::mutex progressMutex;

        auto reportParallelProgress = [&](std::uint64_t transferred, std::string_view currentPath) {
            const auto nowCompleted = parallelCompleted.fetch_add(transferred) + transferred;
            if (progress) {
                std::lock_guard<std::mutex> lock(progressMutex);
                progress(nowCompleted, totalBytes, currentPath);
            }
        };

        auto worker = [&]() {
            std::ifstream sourceInput;
            if (needsSourceArchive) {
                sourceInput.open(sourcePath_, std::ios::binary);
                if (!sourceInput) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!firstError) {
                        firstError = std::make_exception_ptr(
                            Pk2Error("Could not reopen source archive while extracting."));
                    }
                    failed = true;
                    return;
                }
            }

            while (!failed) {
                const auto index = nextIndex.fetch_add(1);
                if (index >= files.size()) {
                    return;
                }
                const auto& file = files[index];
                try {
                    std::ofstream output(file.outputPath, std::ios::binary | std::ios::trunc);
                    if (!output) {
                        throw Pk2Error("Could not create output file: " + pathUtf8(file.outputPath));
                    }
                    copyExtractedFileBytes(output,
                                           file.node->imported ? nullptr : &sourceInput,
                                           *file.node,
                                           file.archivePath,
                                           reportParallelProgress);
                    if (!output) {
                        throw Pk2Error("Could not write output file: " + pathUtf8(file.outputPath));
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!firstError) {
                        firstError = std::current_exception();
                    }
                    failed = true;
                    return;
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& thread : workers) {
            thread.join();
        }

        if (firstError) {
            std::rethrow_exception(firstError);
        }
        completedBytes = parallelCompleted.load();
    }

    if (progress) {
        progress(totalBytes, totalBytes, archivePath);
    }
}

void Pk2Archive::importFile(const fs::path& sourcePath, const std::string& archivePath) {
    importFile(sourcePath, archivePath, {});
}

void Pk2Archive::importFile(const fs::path& sourcePath,
                            const std::string& archivePath,
                            ProgressCallback progress) {
    if (!isRegularFileChecked(sourcePath, "Could not inspect input file")) {
        throw Pk2Error("Input path is not a file: " + pathUtf8(sourcePath));
    }

    const auto normalized = normalizeArchivePath(archivePath);
    const auto name = archiveFileName(normalized);
    if (!isValidEntryName(name) || isReservedEntryName(name)) {
        throw Pk2Error("Invalid PK2 entry name: " + name);
    }

    auto* folder = ensureFolder(archiveParentPath(normalized));
    auto* existing = childByName(*folder, name);
    if (existing != nullptr && existing->isFolder()) {
        throw Pk2Error("A folder already exists at archive path: " + normalized);
    }

    std::uint64_t completed = 0;
    auto fileBytes = readAllBytes(sourcePath,
                                  completed,
                                  fileSizeChecked(sourcePath, "Could not read input file size"),
                                  normalized,
                                  progress);
    Node* target = existing;
    if (target == nullptr) {
        auto node = std::make_unique<Node>();
        node->type = EntryType::File;
        node->name = name;
        node->parent = folder;
        target = node.get();
        folder->children.push_back(std::move(node));
    }

    target->type = EntryType::File;
    target->size = fileBytes.size();
    target->sourceOffset = 0;
    target->importedData = std::move(fileBytes);
    target->imported = true;
    dirty_ = true;
}

void Pk2Archive::importFolder(const fs::path& sourceDirectory, const std::string& archivePath) {
    importFolder(sourceDirectory, archivePath, {});
}

void Pk2Archive::importFolder(const fs::path& sourceDirectory,
                              const std::string& archivePath,
                              ProgressCallback progress) {
    if (!isDirectoryChecked(sourceDirectory, "Could not inspect input folder")) {
        throw Pk2Error("Input path is not a folder: " + pathUtf8(sourceDirectory));
    }

    struct PendingImport {
        fs::path sourcePath;
        std::string archivePath;
        std::uint64_t size{};
    };

    std::vector<PendingImport> pending;
    std::uint64_t totalBytes = 0;
    const auto rootPath = normalizeArchivePath(archivePath);
    ensureFolder(rootPath);
    std::error_code walkError;
    fs::recursive_directory_iterator iterator(sourceDirectory,
                                             fs::directory_options::skip_permission_denied,
                                             walkError);
    if (walkError) {
        throw Pk2Error(filesystemErrorText("Could not scan input folder",
                                           sourceDirectory,
                                           walkError));
    }
    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(walkError)) {
        if (walkError) {
            throw Pk2Error(filesystemErrorText("Could not continue scanning input folder",
                                               sourceDirectory,
                                               walkError));
        }
        const auto& entry = *iterator;
        std::error_code typeError;
        if (!entry.is_regular_file(typeError)) {
            if (typeError) {
                throw Pk2Error(filesystemErrorText("Could not inspect input file",
                                                   entry.path(),
                                                   typeError));
            }
            continue;
        }
        const auto relative = entry.path().lexically_relative(sourceDirectory);
        if (relative.empty()) {
            throw Pk2Error("Could not determine relative import path for: " +
                           pathUtf8(entry.path()));
        }
        const auto target = joinArchivePath(rootPath, pathGenericUtf8(relative));
        std::error_code sizeError;
        const auto size = entry.file_size(sizeError);
        if (sizeError) {
            throw Pk2Error(filesystemErrorText("Could not read input file size",
                                               entry.path(),
                                               sizeError));
        }
        pending.push_back({entry.path(), target, size});
        totalBytes += size;
    }

    std::uint64_t completed = 0;
    if (progress) {
        progress(completed, totalBytes, rootPath);
    }

    for (const auto& item : pending) {
        const auto normalized = normalizeArchivePath(item.archivePath);
        const auto name = archiveFileName(normalized);
        if (!isValidEntryName(name) || isReservedEntryName(name)) {
            throw Pk2Error("Invalid PK2 entry name: " + name);
        }

        auto* folder = ensureFolder(archiveParentPath(normalized));
        auto* existing = childByName(*folder, name);
        if (existing != nullptr && existing->isFolder()) {
            throw Pk2Error("A folder already exists at archive path: " + normalized);
        }

        auto fileBytes = readAllBytes(item.sourcePath,
                                      completed,
                                      totalBytes,
                                      normalized,
                                      progress);
        Node* target = existing;
        if (target == nullptr) {
            auto node = std::make_unique<Node>();
            node->type = EntryType::File;
            node->name = name;
            node->parent = folder;
            target = node.get();
            folder->children.push_back(std::move(node));
        }

        target->type = EntryType::File;
        target->size = fileBytes.size();
        target->sourceOffset = 0;
        target->importedData = std::move(fileBytes);
        target->imported = true;
        dirty_ = true;
    }

    if (progress) {
        progress(totalBytes, totalBytes, rootPath);
    }
}

void Pk2Archive::deleteEntry(const std::string& archivePath) {
    auto* node = findNode(archivePath);
    if (node == nullptr || node == root_.get()) {
        throw Pk2Error("Archive path was not found: " + archivePath);
    }

    // Structural change only: the source file is never touched here, so a
    // delete cannot corrupt the open archive before a .bak backup is taken.
    // save() routes structural changes through the full rewrite path.
    auto* parent = node->parent;
    auto& siblings = parent->children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                                  [&](const auto& child) { return child.get() == node; }),
                   siblings.end());
    dirty_ = true;
    structureDirty_ = true;
}

bool Pk2Archive::saveInPlace() {
    if (sourcePath_.empty() || !existsChecked(sourcePath_, "Could not inspect archive")) {
        return false;
    }

    std::vector<Node*> importedNodes;
    std::function<void(Node&)> collect = [&](Node& node) {
        if (node.imported) {
            importedNodes.push_back(&node);
        }
        for (auto& child : node.children) {
            collect(*child);
        }
    };
    collect(*root_);

    if (structureDirty_) {
        // Folder additions and entry deletions change directory-block layout,
        // which an EOF append cannot express. Fall back to the full rewrite,
        // which also takes a .bak backup of the source archive.
        return false;
    }

    if (importedNodes.empty()) {
        dirty_ = false;
        return true;
    }

    std::fstream file(sourcePath_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return false;
    }

    file.seekp(0, std::ios::end);
    auto currentEof = static_cast<std::uint64_t>(file.tellp());
    if (currentEof < kHeaderSize + kEntryBlockSize) {
        return false;
    }

    auto cipher = makeCipher(password_, cryptoMode_);

    auto writeBlockToDisk = [&](std::uint64_t blockOffset, std::vector<std::uint8_t>& block) {
        if (cipher) {
            cipher->encryptBuffer(block, endianForMode(cryptoMode_));
        }
        file.seekp(static_cast<std::streamoff>(blockOffset), std::ios::beg);
        file.write(reinterpret_cast<const char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        return bool(file);
    };

    auto readBlockFromDisk = [&](std::uint64_t blockOffset) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> block(kEntryBlockSize);
        file.seekg(static_cast<std::streamoff>(blockOffset), std::ios::beg);
        file.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        if (file.gcount() != static_cast<std::streamsize>(block.size())) {
            throw Pk2Error("Could not read block for in-place save.");
        }
        if (cipher) {
            cipher->decryptBuffer(block, endianForMode(cryptoMode_));
        }
        return block;
    };

    auto writeEntryName = [](std::uint8_t* entryBase, const std::string& name) {
        if (name.size() >= kEntryNameSize) {
            throw Pk2Error("PK2 entry name is too long: " + name);
        }
        std::fill_n(entryBase + kEntryNameOffset, kEntryNameSize, 0);
        std::copy_n(name.data(), std::min<std::size_t>(name.size(), kEntryNameSize - 1),
                    entryBase + kEntryNameOffset);
    };

    for (auto* node : importedNodes) {
        if (!node->isFile()) {
            return false;
        }

        file.seekp(static_cast<std::streamoff>(currentEof), std::ios::beg);
        if (!node->importedData.empty()) {
            file.write(reinterpret_cast<const char*>(node->importedData.data()),
                       static_cast<std::streamsize>(node->importedData.size()));
        }
        const auto newDataOffset = currentEof;
        currentEof += node->importedData.size();

        if (node->blockOffset != 0) {
            auto block = readBlockFromDisk(node->blockOffset);
            auto* entryBase = block.data() + node->slotIndex * kEntrySize;
            entryBase[0] = 2;
            writeEntryName(entryBase, node->name);
            writeU64Le(entryBase + kEntryPositionOffset, newDataOffset);
            writeU32Le(entryBase + kEntrySizeOffset, static_cast<std::uint32_t>(node->importedData.size()));
            writeBlockToDisk(node->blockOffset, block);

            node->sourceOffset = newDataOffset;
            node->size = node->importedData.size();
            node->imported = false;
            node->importedData.clear();
            node->importedData.shrink_to_fit();
        } else {
            const auto* parentFolder = node->parent != nullptr ? node->parent : root_.get();
            const auto parentOffset = (parentFolder == root_.get()) ? kHeaderSize : parentFolder->folderFirstBlockOffset;
            if (parentOffset == 0) {
                return false;
            }

            std::uint64_t currBlockOffset = parentOffset;
            bool slotFound = false;
            while (!slotFound) {
                auto block = readBlockFromDisk(currBlockOffset);
                const std::size_t startSlot = (currBlockOffset == parentOffset)
                                                  ? ((parentFolder == root_.get()) ? 1 : 2)
                                                  : 0;
                for (std::size_t s = startSlot; s < kEntriesPerBlock; ++s) {
                    if (block[s * kEntrySize] == 0) {
                        auto* entryBase = block.data() + s * kEntrySize;
                        entryBase[0] = 2;
                        writeEntryName(entryBase, node->name);
                        writeU64Le(entryBase + kEntryPositionOffset, newDataOffset);
                        writeU32Le(entryBase + kEntrySizeOffset, static_cast<std::uint32_t>(node->importedData.size()));
                        writeU64Le(entryBase + kEntryNextBlockOffset, 0);
                        writeBlockToDisk(currBlockOffset, block);

                        node->blockOffset = currBlockOffset;
                        node->slotIndex = s;
                        node->sourceOffset = newDataOffset;
                        node->size = node->importedData.size();
                        node->imported = false;
                        node->importedData.clear();
                        node->importedData.shrink_to_fit();
                        slotFound = true;
                        break;
                    }
                }

                if (slotFound) {
                    break;
                }

                const auto nextChain = readU64Le(block.data() + (kEntriesPerBlock - 1) * kEntrySize + kEntryNextBlockOffset);
                if (nextChain != 0) {
                    currBlockOffset = nextChain;
                } else {
                    const auto newBlockOffset = currentEof;
                    currentEof += kEntryBlockSize;

                    writeU64Le(block.data() + (kEntriesPerBlock - 1) * kEntrySize + kEntryNextBlockOffset, newBlockOffset);
                    writeBlockToDisk(currBlockOffset, block);

                    std::vector<std::uint8_t> newBlock(kEntryBlockSize, 0);
                    auto* entryBase = newBlock.data();
                    entryBase[0] = 2;
                    writeEntryName(entryBase, node->name);
                    writeU64Le(entryBase + kEntryPositionOffset, newDataOffset);
                    writeU32Le(entryBase + kEntrySizeOffset, static_cast<std::uint32_t>(node->importedData.size()));
                    writeU64Le(entryBase + kEntryNextBlockOffset, 0);
                    writeBlockToDisk(newBlockOffset, newBlock);

                    node->blockOffset = newBlockOffset;
                    node->slotIndex = 0;
                    node->sourceOffset = newDataOffset;
                    node->size = node->importedData.size();
                    node->imported = false;
                    node->importedData.clear();
                    node->importedData.shrink_to_fit();
                    slotFound = true;
                    break;
                }
            }
        }
    }

    file.flush();
    dirty_ = false;
    return true;
}

void Pk2Archive::save() {
    if (sourcePath_.empty()) {
        throw Pk2Error("This archive has no source path. Use Save As first.");
    }

    try {
        if (saveInPlace()) {
            return;
        }
    } catch (...) {
    }

    saveDefragmented();
}

void Pk2Archive::saveDefragmented() {
    if (sourcePath_.empty()) {
        throw Pk2Error("This archive has no source path. Use Save As first.");
    }

    const auto originalPath = sourcePath_;

    fs::path backup = originalPath;
    backup += L".bak";
    copyFileChecked(originalPath, backup);

    fs::path temp = originalPath;
    temp += L".tmp";
    try {
        writeArchive(temp);
        auto reopened = Pk2Archive::open(temp, password_);
        try {
            removeChecked(originalPath);
            renameChecked(temp, originalPath);
        } catch (...) {
            if (!existsChecked(originalPath, "Could not inspect source archive") &&
                existsChecked(backup, "Could not inspect backup archive")) {
                copyFileChecked(backup, originalPath);
            }
            throw;
        }
        reopened.sourcePath_ = originalPath;
        *this = std::move(reopened);
    } catch (...) {
        try {
            removeChecked(temp);
        } catch (...) {
        }
        throw;
    }
}

void Pk2Archive::saveAs(const fs::path& outputPath) {
    fs::path temp = outputPath;
    temp += L".tmp";
    try {
        writeArchive(temp);
        auto reopened = Pk2Archive::open(temp, password_);
        if (existsChecked(outputPath, "Could not inspect output archive")) {
            removeChecked(outputPath);
        }
        renameChecked(temp, outputPath);
        reopened.sourcePath_ = outputPath;
        *this = std::move(reopened);
    } catch (...) {
        try {
            removeChecked(temp);
        } catch (...) {
        }
        throw;
    }
}

void Pk2Archive::writeArchive(const fs::path& outputPath) const {
    struct BlockPlan {
        const Node* folder{};
        bool isRoot{};
        std::size_t firstChild{};
        std::uint64_t offset{};
        std::uint64_t nextOffset{};
    };

    std::vector<BlockPlan> blocks;
    std::map<const Node*, std::uint64_t> folderBlocks;

    std::function<void(const Node&)> allocateFolder = [&](const Node& folder) {
        const bool isRoot = (&folder == root_.get());
        const auto childCount = folder.children.size();
        const std::size_t firstBlockCapacity = isRoot ? 19 : 18;

        std::size_t blockCount = 1;
        if (childCount > firstBlockCapacity) {
            blockCount = 1 + (childCount - firstBlockCapacity + (kEntriesPerBlock - 1)) / kEntriesPerBlock;
        }

        const auto firstIndex = blocks.size();
        for (std::size_t i = 0; i < blockCount; ++i) {
            const auto offset = static_cast<std::uint64_t>(kHeaderSize + blocks.size() * kEntryBlockSize);
            const std::size_t firstChild = (i == 0) ? 0 : (firstBlockCapacity + (i - 1) * kEntriesPerBlock);
            blocks.push_back({&folder, isRoot, firstChild, offset, 0});
        }

        folderBlocks[&folder] = blocks[firstIndex].offset;
        for (std::size_t i = 0; i + 1 < blockCount; ++i) {
            blocks[firstIndex + i].nextOffset = blocks[firstIndex + i + 1].offset;
        }

        for (const auto& child : folder.children) {
            if (child->isFolder()) {
                allocateFolder(*child);
            }
        }
    };
    allocateFolder(*root_);

    std::vector<const Node*> files;
    std::function<void(const Node&)> collectFiles = [&](const Node& node) {
        if (node.isFile()) {
            files.push_back(&node);
            return;
        }
        for (const auto& child : node.children) {
            collectFiles(*child);
        }
    };
    collectFiles(*root_);

    std::map<const Node*, std::uint64_t> fileOffsets;
    auto nextDataOffset = static_cast<std::uint64_t>(kHeaderSize + blocks.size() * kEntryBlockSize);
    for (const auto* file : files) {
        if (file->size > 0xffffffffull) {
            throw Pk2Error("PK2 v1 writer only supports files up to 4 GB.");
        }
        fileOffsets[file] = nextDataOffset;
        nextDataOffset += file->size;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw Pk2Error("Could not create output archive: " + pathUtf8(outputPath));
    }

    std::array<std::uint8_t, kHeaderSize> header{};
    if (sourceHeader_.size() == kHeaderSize) {
        std::copy(sourceHeader_.begin(), sourceHeader_.end(), header.begin());
        writeU32Le(header.data() + 30, 0x01000002);
        header[34] = isEncryptedMode(cryptoMode_) ? 1 : 0;
    } else {
        const std::string headerName = "JoyMax File Manager!\n";
        std::copy(headerName.begin(), headerName.end(), header.begin());
        writeU32Le(header.data() + 30, 0x01000002);
        header[34] = isEncryptedMode(cryptoMode_) ? 1 : 0;
    }

    auto cipher = makeCipher(password_, cryptoMode_);

    if (isEncryptedMode(cryptoMode_) && cipher != nullptr) {
        std::vector<std::uint8_t> verifyBlock = {
            'J', 'o', 'y', 'm', 'a', 'x', ' ', 'P',
            'a', 'k', ' ', 'F', 'i', 'l', 'e', 0
        };
        cipher->encryptBuffer(verifyBlock, endianForMode(cryptoMode_));
        std::copy_n(verifyBlock.begin(), std::min<std::size_t>(3, verifyBlock.size()), header.begin() + 35);
        std::fill_n(header.begin() + 38, 13, 0);
    }
    output.write(reinterpret_cast<const char*>(header.data()), header.size());

    const auto writeName = [](std::uint8_t* entryBase, const std::string& name) {
        if (name.size() >= kEntryNameSize) {
            throw Pk2Error("PK2 entry name is too long: " + name);
        }
        std::copy(name.begin(), name.end(), entryBase + kEntryNameOffset);
    };

    for (const auto& plan : blocks) {
        std::vector<std::uint8_t> block(kEntryBlockSize, 0);
        const auto& children = plan.folder->children;
        std::size_t slot = 0;

        if (plan.firstChild == 0) {
            if (plan.isRoot) {
                // Root directory entry "."
                auto* entryBase = block.data() + slot * kEntrySize;
                entryBase[0] = 1;
                writeName(entryBase, ".");
                writeU64Le(entryBase + kEntryPositionOffset, folderBlocks.at(plan.folder));
                writeU32Le(entryBase + kEntrySizeOffset, 0);
                writeU64Le(entryBase + kEntryNextBlockOffset, 0);
                slot = 1;
            } else {
                // Current directory entry "."
                auto* entryBase = block.data() + slot * kEntrySize;
                entryBase[0] = 1;
                writeName(entryBase, ".");
                writeU64Le(entryBase + kEntryPositionOffset, folderBlocks.at(plan.folder));
                writeU32Le(entryBase + kEntrySizeOffset, 0);
                writeU64Le(entryBase + kEntryNextBlockOffset, 0);

                // Parent directory entry ".."
                const auto* parentFolder = plan.folder->parent != nullptr ? plan.folder->parent : root_.get();
                auto* parentEntryBase = block.data() + 1 * kEntrySize;
                parentEntryBase[0] = 1;
                writeName(parentEntryBase, "..");
                writeU64Le(parentEntryBase + kEntryPositionOffset, folderBlocks.at(parentFolder));
                parentEntryBase[kEntrySizeOffset] = 0;
                writeU64Le(parentEntryBase + kEntryNextBlockOffset, 0);

                slot = 2;
            }
        }

        std::size_t childIndex = plan.firstChild;
        while (slot < kEntriesPerBlock && childIndex < children.size()) {
            const auto& child = *children[childIndex];
            auto* entryBase = block.data() + slot * kEntrySize;
            entryBase[0] = child.isFolder() ? 1 : 2;
            writeName(entryBase, child.name);
            if (child.isFolder()) {
                writeU64Le(entryBase + kEntryPositionOffset, folderBlocks.at(&child));
                writeU32Le(entryBase + kEntrySizeOffset, 0);
            } else {
                writeU64Le(entryBase + kEntryPositionOffset, fileOffsets.at(&child));
                writeU32Le(entryBase + kEntrySizeOffset, static_cast<std::uint32_t>(child.size));
            }
            writeU64Le(entryBase + kEntryNextBlockOffset, 0);

            ++slot;
            ++childIndex;
        }

        if (plan.nextOffset != 0) {
            writeU64Le(block.data() + (kEntriesPerBlock - 1) * kEntrySize + kEntryNextBlockOffset,
                       plan.nextOffset);
        }

        if (cipher) {
            cipher->encryptBuffer(block, endianForMode(cryptoMode_));
        }
        output.write(reinterpret_cast<const char*>(block.data()),
                     static_cast<std::streamsize>(block.size()));
    }

    for (const auto* file : files) {
        appendFileBytes(output, sourcePath_, *file);
    }

    if (!output) {
        throw Pk2Error("Failed while writing output archive.");
    }
}

} // namespace pk2

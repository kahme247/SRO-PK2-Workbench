#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pk2/crypto.h"

namespace pk2 {

class Pk2Error : public std::runtime_error {
public:
    explicit Pk2Error(const std::string& message) : std::runtime_error(message) {}
};

enum class EntryType {
    Folder,
    File
};

enum class OverwritePolicy {
    Fail,
    Replace
};

struct EntryInfo {
    EntryType type{};
    std::string path;
    std::string name;
    std::uint64_t size{};
    std::uint64_t dataOffset{};
};

using ProgressCallback = std::function<void(std::uint64_t completed,
                                            std::uint64_t total,
                                            std::string_view currentPath)>;

class Pk2Archive {
public:
    struct Node;
    enum class CryptoMode : std::uint8_t;

    Pk2Archive();
    ~Pk2Archive();
    Pk2Archive(Pk2Archive&&) noexcept;
    Pk2Archive& operator=(Pk2Archive&&) noexcept;

    Pk2Archive(const Pk2Archive&) = delete;
    Pk2Archive& operator=(const Pk2Archive&) = delete;

    static Pk2Archive createNew(std::string password = std::string(kOfficialSroPassword));
    static Pk2Archive open(const std::filesystem::path& path,
                           std::string password = std::string(kOfficialSroPassword));

    const std::filesystem::path& sourcePath() const;
    const std::string& password() const;
    bool dirty() const;
    bool empty() const;
    std::size_t entryCount() const;

    std::vector<EntryInfo> listTree() const;
    std::vector<EntryInfo> children(const std::string& archiveFolderPath) const;
    std::optional<EntryInfo> find(const std::string& archivePath) const;
    std::vector<std::uint8_t> readFile(const std::string& archivePath) const;
    void importFileBytes(std::vector<std::uint8_t> bytes,
                         const std::string& archivePath);

    void extract(const std::string& archivePath,
                 const std::filesystem::path& destination,
                 bool recurse,
                 OverwritePolicy overwrite) const;
    void extract(const std::string& archivePath,
                 const std::filesystem::path& destination,
                 bool recurse,
                 OverwritePolicy overwrite,
                 ProgressCallback progress) const;

    void importFile(const std::filesystem::path& sourcePath,
                    const std::string& archivePath);
    void importFile(const std::filesystem::path& sourcePath,
                    const std::string& archivePath,
                    ProgressCallback progress);
    void importFolder(const std::filesystem::path& sourceDirectory,
                      const std::string& archivePath);
    void importFolder(const std::filesystem::path& sourceDirectory,
                      const std::string& archivePath,
                      ProgressCallback progress);
    void deleteEntry(const std::string& archivePath);

    void save();
    void saveDefragmented();
    void saveAs(const std::filesystem::path& outputPath);

private:
    friend struct ArchiveIo;

    explicit Pk2Archive(std::unique_ptr<Node> root, std::string password);

    Node* findNode(const std::string& archivePath);
    const Node* findNode(const std::string& archivePath) const;
    Node* ensureFolder(const std::string& archivePath);
    EntryInfo makeInfo(const Node& node) const;
    bool saveInPlace();
    void writeArchive(const std::filesystem::path& outputPath) const;

    std::unique_ptr<Node> root_;
    std::filesystem::path sourcePath_;
    std::vector<std::uint8_t> sourceHeader_;
    std::string password_;
    CryptoMode cryptoMode_;
    bool dirty_{false};
    // Set by structural changes (entry deletion, new folders) that alter
    // directory-block layout and cannot be persisted by an in-place append.
    bool structureDirty_{false};
};

} // namespace pk2

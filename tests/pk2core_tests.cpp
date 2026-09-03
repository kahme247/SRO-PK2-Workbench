#include "pk2/archive.h"
#include "pk2/crypto.h"
#include "pk2/path.h"
#include "pk2/server_config.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> readPrefix(const fs::path& path, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<std::uint8_t> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(input.gcount() == static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

fs::path testRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("clean_pk2_tests_" + std::to_string(stamp));
}

void testMd5() {
    assert(pk2::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    assert(pk2::md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
}

void testBlowfishKnownVector() {
    const std::uint8_t zeroKey[8]{};
    std::uint8_t block[8]{};
    pk2::Blowfish cipher(zeroKey, sizeof(zeroKey));
    cipher.encryptBlock(block, pk2::BlockEndian::Big);
    const std::uint8_t expected[8] = {0x4e, 0xf9, 0x97, 0x45, 0x61, 0x98, 0xdd, 0x78};
    for (std::size_t i = 0; i < 8; ++i) {
        assert(block[i] == expected[i]);
    }
    cipher.decryptBlock(block, pk2::BlockEndian::Big);
    for (const auto byte : block) {
        assert(byte == 0);
    }
}

void testArchiveRoundTrip() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto extracted = root / "extracted";
    const auto archivePath = root / "roundtrip.pk2";

    writeText(source / "hello.txt", "hello pk2");
    writeText(source / "nested" / "world.txt", "nested data");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source / "hello.txt", "Data/hello.txt");
    archive.importFolder(source / "nested", "Data/nested");
    archive.saveAs(archivePath);
    const auto originalHeader = readPrefix(archivePath, 256);
    assert(originalHeader[30] == 2);
    assert(originalHeader[31] == 0);
    assert(originalHeader[32] == 0);
    assert(originalHeader[33] == 1);
    assert(originalHeader[34] == 1);

    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    const auto entries = reopened.listTree();
    assert(entries.size() == 4);
    assert(reopened.find("Data/hello.txt").has_value());
    assert(reopened.find("Data/nested/world.txt").has_value());
    const auto helloBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(helloBytes.begin(), helloBytes.end()) == "hello pk2");
    reopened.importFileBytes({'u', 'p', 'd', 'a', 't', 'e', 'd'}, "Data/hello.txt");
    const auto updatedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(updatedBytes.begin(), updatedBytes.end()) == "updated");
    reopened.save();
    assert(readPrefix(archivePath, 256) == originalHeader);
    const auto savedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(savedBytes.begin(), savedBytes.end()) == "updated");

    reopened.importFileBytes({'u', 'p', 'd', 'a', 't', 'e', 'd', ' ', 'a', 'g', 'a', 'i', 'n'},
                             "Data/hello.txt");
    reopened.save();
    reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(readPrefix(archivePath, 256) == originalHeader);
    const auto twiceSavedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(twiceSavedBytes.begin(), twiceSavedBytes.end()) == "updated again");
    const auto untouchedBytes = reopened.readFile("Data/nested/world.txt");
    assert(std::string(untouchedBytes.begin(), untouchedBytes.end()) == "nested data");

    reopened.extract("Data", extracted, true, pk2::OverwritePolicy::Replace);
    assert(readText(extracted / "Data" / "hello.txt") == "updated again");
    assert(readText(extracted / "Data" / "nested" / "world.txt") == "nested data");

    reopened.deleteEntry("Data/hello.txt");
    assert(!reopened.find("Data/hello.txt").has_value());

    fs::remove_all(root);
}

void testServerConfigRoundTrip() {
    pk2::ServerConfig original;
    original.contentId = 22;
    original.version = 188;
    original.port = 15779;
    original.versionEndian = pk2::BlockEndian::Little;
    original.divisions = {
        {"Private SRO", {"127.0.0.1", "gateway.example.com"}},
        {"Second Division", {"10.0.0.25"}},
    };

    const auto divisionInfo = pk2::serializeDivisionInfo(original);
    const auto gatePort = pk2::serializeGatePort(original);
    const auto version = pk2::serializeServerVersion(original);
    assert(gatePort.size() == 8);
    assert(version.size() == 1024);
    assert(std::all_of(version.begin() + 12, version.end(), [](std::uint8_t byte) {
        return byte == 0;
    }));

    const auto parsed = pk2::parseServerConfig(divisionInfo, gatePort, version);
    assert(parsed.contentId == 22);
    assert(parsed.version == 188);
    assert(parsed.port == 15779);
    assert(parsed.versionEndian == pk2::BlockEndian::Little);
    assert(parsed.versionBlockAtOffset4);
    assert(parsed.divisions.size() == 2);
    assert(parsed.divisions[0].name == "Private SRO");
    assert(parsed.divisions[0].gateways.size() == 2);
    assert(parsed.divisions[0].gateways[1] == "gateway.example.com");
    assert(parsed.divisions[1].gateways[0] == "10.0.0.25");

    original.versionEndian = pk2::BlockEndian::Big;
    const auto bigEndianVersion = pk2::serializeServerVersion(original);
    const auto parsedBig = pk2::parseServerConfig(divisionInfo, gatePort, bigEndianVersion);
    assert(parsedBig.version == 188);
    assert(parsedBig.versionEndian == pk2::BlockEndian::Big);

    original.versionFile.assign(1024, 0);
    original.versionFile[0] = 0x53;
    original.versionFile[100] = 0x7a;
    const auto preservedVersion = pk2::serializeServerVersion(original);
    const auto parsedPreserved = pk2::parseServerConfig(divisionInfo, gatePort, preservedVersion);
    assert(parsedPreserved.versionFile[0] == 0x53);
    assert(parsedPreserved.versionFile[100] == 0x7a);

    // Test resilient parsing when only divisionInfo is present
    const auto parsedDivisionOnly = pk2::parseServerConfig(divisionInfo, {}, {});
    assert(parsedDivisionOnly.contentId == 22);
    assert(parsedDivisionOnly.port == 15779); // default port
    assert(parsedDivisionOnly.version == 188); // default version
    assert(parsedDivisionOnly.divisions.size() == 2);

    // Test resilient parsing with empty inputs (template defaults)
    const auto parsedDefaults = pk2::parseServerConfig({}, {}, {});
    assert(parsedDefaults.contentId == 22);
    assert(parsedDefaults.port == 15779);
    assert(parsedDefaults.version == 188);
    assert(parsedDefaults.divisions.size() == 1);
    assert(parsedDefaults.divisions[0].name == "Silkroad");
    assert(parsedDefaults.divisions[0].gateways[0] == "127.0.0.1");
}

void testMultiBlockArchiveRoundTrip() {
    const auto root = testRoot();
    const auto source = root / "bulk-source";
    const auto archivePath = root / "bulk.pk2";

    auto archive = pk2::Pk2Archive::createNew("169841");
    for (int i = 0; i < 45; ++i) {
        const auto fileName = "file_" + std::to_string(i) + ".txt";
        const auto sourcePath = source / fileName;
        writeText(sourcePath, "bulk " + std::to_string(i));
        archive.importFile(sourcePath, "Bulk/" + fileName);
    }
    archive.saveAs(archivePath);

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(reopened.find("Bulk/file_0.txt").has_value());
    assert(reopened.find("Bulk/file_44.txt").has_value());
    assert(reopened.children("Bulk").size() == 45);

    fs::remove_all(root);
}

void testCanonicalRootBlockWinsOverHeaderScanCandidate() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto archivePath = root / "ambiguous-root.pk2";

    auto archive = pk2::Pk2Archive::createNew("169841");
    for (int i = 0; i < 25; ++i) {
        const auto fileName = "file_" + std::to_string(i) + ".txt";
        const auto sourcePath = source / fileName;
        writeText(sourcePath, "nested " + std::to_string(i));
        archive.importFile(sourcePath, "folder/" + fileName);
    }
    archive.saveAs(archivePath);

    {
        std::fstream file(archivePath, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        const std::uint32_t childBlockOffset = 256 + 2560;
        std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(childBlockOffset & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 8) & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 16) & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 24) & 0xff),
        };
        file.seekp(24, std::ios::beg);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    const auto rootChildren = reopened.children("");
    assert(rootChildren.size() == 1);
    assert(rootChildren[0].type == pk2::EntryType::Folder);
    assert(rootChildren[0].path == "folder");
    assert(reopened.find("folder/file_0.txt").has_value());

    fs::remove_all(root);
}

void testUnicodeFilesystemPathUtf8() {
#ifdef _WIN32
    const fs::path path = fs::path(L"C:\\pk2") /
                          L"\u062A\u062C\u0631\u0628\u0629" /
                          L"\u6D4B\u8BD5.txt";
    const auto text = pk2::pathUtf8(path);
    assert(text.find("C:\\pk2") != std::string::npos);
    assert(text.find("\xD8\xAA\xD8\xAC\xD8\xB1\xD8\xA8\xD8\xA9") != std::string::npos);
    assert(text.find("\xE6\xB5\x8B\xE8\xAF\x95.txt") != std::string::npos);
#endif
}

void testUnicodeFilesystemArchiveRoundTrip() {
#ifdef _WIN32
    const auto root = testRoot() / L"\u062A\u062C\u0631\u0628\u0629";
    const auto source = root / L"\u6D4B\u8BD5-source";
    const auto extracted = root / L"\u062E\u0631\u0648\u062C";
    const auto archivePath = root / L"\u0645\u0644\u0641.pk2";

    writeText(source / L"\u6D4B\u8BD5.txt", "unicode file");
    writeText(source / L"\u0641\u0631\u0639" / L"nested.txt", "unicode nested");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFolder(source, "Unicode");
    archive.saveAs(archivePath);

    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(reopened.find("Unicode/\xE6\xB5\x8B\xE8\xAF\x95.txt").has_value());
    reopened.extract("Unicode", extracted, true, pk2::OverwritePolicy::Replace);
    assert(readText(extracted / "Unicode" / L"\u6D4B\u8BD5.txt") == "unicode file");
    assert(readText(extracted / "Unicode" / L"\u0641\u0631\u0639" / "nested.txt") == "unicode nested");

    fs::remove_all(root);
#endif
}

void testLegacyByteArchiveNameExtraction() {
#ifdef _WIN32
    const auto root = testRoot();
    const auto source = root / "source.txt";
    const auto archivePath = root / "legacy-byte-name.pk2";
    const auto extracted = root / "extracted";
    const std::string rawName = std::string("legacy_") + static_cast<char>(0xe9) + ".txt";

    writeText(source, "legacy byte name");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source, "Raw/" + rawName);
    archive.saveAs(archivePath);

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    reopened.extract("Raw", extracted, true, pk2::OverwritePolicy::Replace);
    const auto expectedPath = extracted / "Raw" / pk2::archivePathPartToFilesystem(rawName);
    assert(readText(expectedPath) == "legacy byte name");

    fs::remove_all(root);
#endif
}

void testDirectoryBlockDotAndDotDotStructure() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto archivePath = root / "dot_test.pk2";

    writeText(source / "file1.txt", "file 1 content");
    writeText(source / "sub" / "file2.txt", "file 2 content");
    fs::create_directories(source / "empty_dir");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source / "file1.txt", "file1.txt");
    archive.importFile(source / "sub" / "file2.txt", "sub/file2.txt");
    archive.importFolder(source / "empty_dir", "empty_dir");
    archive.saveAs(archivePath);

    std::vector<std::uint8_t> data;
    {
        std::ifstream file(archivePath, std::ios::binary);
        assert(file);
        data.assign((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
    }
    assert(data.size() >= 256 + 2560);

    pk2::Blowfish cipher("169841", pk2::KeyScheduleMode::JoymaxCompatible);

    // Header checks
    assert(data[30] == 2 && data[31] == 0 && data[32] == 0 && data[33] == 1);
    assert(data[34] == 1); // encrypted

    // Decrypt root block
    std::vector<std::uint8_t> rootBlock(data.begin() + 256, data.begin() + 256 + 2560);
    cipher.decryptBuffer(rootBlock, pk2::BlockEndian::Little);

    // Slot 0 of root must be "."
    assert(rootBlock[0] == 1); // folder
    assert(std::string(reinterpret_cast<char*>(rootBlock.data() + 1)) == ".");

    // Decrypt sub block (offset 256 + 2560)
    std::vector<std::uint8_t> subBlock(data.begin() + 256 + 2560, data.begin() + 256 + 2560 * 2);
    cipher.decryptBuffer(subBlock, pk2::BlockEndian::Little);

    // Slot 0 of sub must be ".", slot 1 must be ".."
    assert(subBlock[0] == 1);
    assert(std::string(reinterpret_cast<char*>(subBlock.data() + 1)) == ".");
    assert(subBlock[128] == 1);
    assert(std::string(reinterpret_cast<char*>(subBlock.data() + 128 + 1)) == "..");

    // Decrypt empty_dir block (offset 256 + 2560 * 2)
    std::vector<std::uint8_t> emptyBlock(data.begin() + 256 + 2560 * 2, data.begin() + 256 + 2560 * 3);
    cipher.decryptBuffer(emptyBlock, pk2::BlockEndian::Little);
    assert(emptyBlock[0] == 1);
    assert(std::string(reinterpret_cast<char*>(emptyBlock.data() + 1)) == ".");
    assert(emptyBlock[128] == 1);
    assert(std::string(reinterpret_cast<char*>(emptyBlock.data() + 128 + 1)) == "..");

    fs::remove_all(root);
}

void testInPlaceQuickSaveAndDefragment() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto archivePath = root / "inplace.pk2";

    writeText(source / "file1.txt", "initial file 1 content");
    writeText(source / "sub" / "file2.txt", "initial file 2 content");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source / "file1.txt", "file1.txt");
    archive.importFile(source / "sub" / "file2.txt", "sub/file2.txt");
    archive.saveAs(archivePath);

    const auto sizeBefore = fs::file_size(archivePath);

    // Open and perform in-place update of file1.txt
    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    const std::string newContent = "a much longer updated file 1 content that is appended";
    reopened.importFileBytes(std::vector<std::uint8_t>(newContent.begin(), newContent.end()), "file1.txt");
    reopened.save(); // In-place quick save

    const auto sizeAfterInPlace = fs::file_size(archivePath);
    // In-place save appends new payload to EOF without rewriting earlier blocks
    assert(sizeAfterInPlace == sizeBefore + newContent.size());

    // Verify content can be read correctly
    auto verifyArchive = pk2::Pk2Archive::open(archivePath, "169841");
    const auto readBytes = verifyArchive.readFile("file1.txt");
    assert(std::string(readBytes.begin(), readBytes.end()) == newContent);
    const auto unchangedBytes = verifyArchive.readFile("sub/file2.txt");
    assert(std::string(unchangedBytes.begin(), unchangedBytes.end()) == "initial file 2 content");

    // Defragmenting should remove orphaned old file1 content
    verifyArchive.saveDefragmented();
    const auto sizeAfterDefrag = fs::file_size(archivePath);
    assert(sizeAfterDefrag < sizeAfterInPlace);

    auto finalCheck = pk2::Pk2Archive::open(archivePath, "169841");
    const auto finalBytes = finalCheck.readFile("file1.txt");
    assert(std::string(finalBytes.begin(), finalBytes.end()) == newContent);

    fs::remove_all(root);
}

void testEmptyArchiveCreateAndReopen() {
    const auto root = testRoot();
    fs::create_directories(root);
    const auto archivePath = root / "empty.pk2";

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.saveAs(archivePath);
    assert(fs::exists(archivePath));
    assert(!fs::exists(fs::path(archivePath.string() + ".tmp")));

    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(reopened.empty());
    assert(reopened.entryCount() == 0);
    assert(reopened.listTree().empty());

    // A reopened empty archive accepts imports into new folders.
    writeText(root / "hello.txt", "hello empty archive");
    reopened.importFile(root / "hello.txt", "docs/hello.txt");
    reopened.save();
    const auto check = pk2::Pk2Archive::open(archivePath, "169841");
    const auto bytes = check.readFile("docs/hello.txt");
    assert(std::string(bytes.begin(), bytes.end()) == "hello empty archive");

    fs::remove_all(root);
}

void testDeletePersistsThroughSave() {
    const auto root = testRoot();
    const auto archivePath = root / "delete.pk2";

    writeText(root / "a.txt", "aaa");
    writeText(root / "b.txt", "bbb");
    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(root / "a.txt", "a.txt");
    archive.importFile(root / "b.txt", "sub/b.txt");
    archive.saveAs(archivePath);

    auto opened = pk2::Pk2Archive::open(archivePath, "169841");
    opened.deleteEntry("sub/b.txt");
    opened.save();

    // Structural saves rewrite through the .bak-backed path.
    assert(fs::exists(fs::path(archivePath.string() + ".bak")));

    auto check = pk2::Pk2Archive::open(archivePath, "169841");
    assert(!check.find("sub/b.txt").has_value());
    const auto kept = check.readFile("a.txt");
    assert(std::string(kept.begin(), kept.end()) == "aaa");

    fs::remove_all(root);
}

void testSaveAsFailureLeavesNoTemp() {
    const auto root = testRoot();
    fs::create_directories(root);
    auto archive = pk2::Pk2Archive::createNew("169841");
    bool threw = false;
    try {
        archive.saveAs(root / "no-such-dir" / "out.pk2");
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    assert(!fs::exists(root / "no-such-dir" / "out.pk2.tmp"));

    fs::remove_all(root);
}

} // namespace

int main() {
    testMd5();
    testEmptyArchiveCreateAndReopen();
    testDeletePersistsThroughSave();
    testSaveAsFailureLeavesNoTemp();
    testBlowfishKnownVector();
    testArchiveRoundTrip();
    testServerConfigRoundTrip();
    testMultiBlockArchiveRoundTrip();
    testCanonicalRootBlockWinsOverHeaderScanCandidate();
    testDirectoryBlockDotAndDotDotStructure();
    testInPlaceQuickSaveAndDefragment();
    testUnicodeFilesystemPathUtf8();
    testUnicodeFilesystemArchiveRoundTrip();
    testLegacyByteArchiveNameExtraction();
    return 0;
}

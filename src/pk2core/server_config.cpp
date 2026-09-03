#include "pk2/server_config.h"

#include "pk2/archive.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace pk2 {
namespace {

constexpr std::size_t kVersionFileSize = 1024;
constexpr std::string_view kVersionKey = "SILKROAD";

std::uint32_t readU32Le(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw Pk2Error("DIVISIONINFO.TXT is truncated.");
    }
    const auto value = static_cast<std::uint32_t>(bytes[offset]) |
                       (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                       (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                       (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return value;
}

void writeU32Le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

std::string readSizedString(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    const auto length = readU32Le(bytes, offset);
    if (length > 4096 || offset > bytes.size() || bytes.size() - offset < length + 1) {
        throw Pk2Error("DIVISIONINFO.TXT contains an invalid string length.");
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    if (bytes[offset++] != 0) {
        throw Pk2Error("DIVISIONINFO.TXT contains an unterminated string.");
    }
    return value;
}

void writeSizedString(std::vector<std::uint8_t>& bytes, const std::string& value) {
    if (value.empty()) {
        throw Pk2Error("Division names and gateway URLs cannot be empty.");
    }
    if (value.size() > 4096 || value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Pk2Error("A division name or gateway URL is too long.");
    }
    if (value.find('\0') != std::string::npos) {
        throw Pk2Error("Division names and gateway URLs cannot contain null bytes.");
    }
    writeU32Le(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

std::uint32_t parseDecimal(std::string_view value, const char* label) {
    if (value.empty()) {
        throw Pk2Error(std::string(label) + " is empty.");
    }
    std::uint64_t number = 0;
    for (const auto ch : value) {
        if (ch < '0' || ch > '9') {
            throw Pk2Error(std::string(label) + " is not a decimal number.");
        }
        number = number * 10 + static_cast<unsigned int>(ch - '0');
        if (number > std::numeric_limits<std::uint32_t>::max()) {
            throw Pk2Error(std::string(label) + " is too large.");
        }
    }
    return static_cast<std::uint32_t>(number);
}

std::string decimalPrefix(const std::uint8_t* begin, std::size_t length) {
    std::string value;
    for (std::size_t i = 0; i < length && begin[i] != 0; ++i) {
        if (std::isdigit(begin[i]) == 0) {
            return {};
        }
        value.push_back(static_cast<char>(begin[i]));
    }
    return value;
}

std::string decryptedVersionBlock(const std::vector<std::uint8_t>& bytes, BlockEndian endian) {
    if (bytes.size() < 12) {
        return {};
    }
    std::uint8_t block[8]{};
    std::copy_n(bytes.data() + 4, 8, block);
    Blowfish cipher(kVersionKey);
    cipher.decryptBlock(block, endian);
    return decimalPrefix(block, 4);
}

std::string decryptedLegacyVersionText(std::vector<std::uint8_t> bytes, BlockEndian endian) {
    Blowfish cipher(kVersionKey);
    cipher.decryptBuffer(bytes, endian);
    const auto terminator = std::find(bytes.begin(), bytes.end(), 0);
    if (terminator == bytes.end()) {
        return {};
    }
    std::string value(bytes.begin(), terminator);
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        return {};
    }
    return value;
}

} // namespace

ServerConfig parseServerConfig(const std::vector<std::uint8_t>& divisionInfo,
                               const std::vector<std::uint8_t>& gatePort,
                               const std::vector<std::uint8_t>& encryptedVersion) {
    ServerConfig config;
    config.contentId = 22;
    config.port = 15779;
    config.version = 188;

    if (!divisionInfo.empty()) {
        if (divisionInfo.size() < 2) {
            throw Pk2Error("DIVISIONINFO.TXT is truncated.");
        }

        std::size_t offset = 0;
        config.contentId = divisionInfo[offset++];
        const auto divisionCount = divisionInfo[offset++];
        for (std::size_t i = 0; i < divisionCount; ++i) {
            ServerDivision division;
            division.name = readSizedString(divisionInfo, offset);
            if (offset >= divisionInfo.size()) {
                throw Pk2Error("DIVISIONINFO.TXT is missing a gateway count.");
            }
            const auto gatewayCount = divisionInfo[offset++];
            for (std::size_t gateway = 0; gateway < gatewayCount; ++gateway) {
                division.gateways.push_back(readSizedString(divisionInfo, offset));
            }
            config.divisions.push_back(std::move(division));
        }
    }

    if (config.divisions.empty()) {
        config.divisions = {{"Silkroad", {"127.0.0.1"}}};
    }

    if (!gatePort.empty()) {
        std::string portText;
        for (const auto byte : gatePort) {
            if (byte == 0 || std::isspace(byte) != 0) {
                continue;
            }
            portText.push_back(static_cast<char>(byte));
        }
        if (!portText.empty()) {
            const auto port = parseDecimal(portText, "GATEPORT.TXT");
            if (port == 0 || port > 65535) {
                throw Pk2Error("GATEPORT.TXT must contain a port from 1 to 65535.");
            }
            config.port = static_cast<std::uint16_t>(port);
        }
    }

    if (!encryptedVersion.empty()) {
        if (encryptedVersion.size() != kVersionFileSize) {
            throw Pk2Error("SV.T must be exactly 1024 bytes.");
        }
        config.versionFile = encryptedVersion;
        auto versionText = decryptedVersionBlock(encryptedVersion, BlockEndian::Little);
        if (!versionText.empty()) {
            config.versionEndian = BlockEndian::Little;
        } else {
            versionText = decryptedVersionBlock(encryptedVersion, BlockEndian::Big);
            if (!versionText.empty()) {
                config.versionEndian = BlockEndian::Big;
            } else {
                config.versionBlockAtOffset4 = false;
                versionText = decryptedLegacyVersionText(encryptedVersion, BlockEndian::Little);
                if (!versionText.empty()) {
                    config.versionEndian = BlockEndian::Little;
                } else {
                    versionText = decryptedLegacyVersionText(encryptedVersion, BlockEndian::Big);
                    if (versionText.empty()) {
                        throw Pk2Error("SV.T could not be decrypted with the SILKROAD version key.");
                    }
                    config.versionEndian = BlockEndian::Big;
                }
            }
        }
        config.version = parseDecimal(versionText, "SV.T version");
    } else {
        config.versionFile.assign(kVersionFileSize, 0);
    }
    return config;
}

std::vector<std::uint8_t> serializeDivisionInfo(const ServerConfig& config) {
    if (config.divisions.empty() || config.divisions.size() > 255) {
        throw Pk2Error("Server configuration must contain 1 to 255 divisions.");
    }
    std::vector<std::uint8_t> bytes;
    bytes.push_back(config.contentId);
    bytes.push_back(static_cast<std::uint8_t>(config.divisions.size()));
    for (const auto& division : config.divisions) {
        writeSizedString(bytes, division.name);
        if (division.gateways.empty() || division.gateways.size() > 255) {
            throw Pk2Error("Each division must contain 1 to 255 gateway URLs.");
        }
        bytes.push_back(static_cast<std::uint8_t>(division.gateways.size()));
        for (const auto& gateway : division.gateways) {
            writeSizedString(bytes, gateway);
        }
    }
    return bytes;
}

std::vector<std::uint8_t> serializeGatePort(const ServerConfig& config) {
    if (config.port == 0) {
        throw Pk2Error("Gateway port must be from 1 to 65535.");
    }
    const auto text = std::to_string(config.port);
    std::vector<std::uint8_t> bytes(8, 0);
    std::copy(text.begin(), text.end(), bytes.begin());
    return bytes;
}

std::vector<std::uint8_t> serializeServerVersion(const ServerConfig& config) {
    const auto text = std::to_string(config.version);
    if (config.versionBlockAtOffset4 && text.size() > 4) {
        throw Pk2Error("SV.T version must be between 0 and 9999.");
    }
    auto bytes = config.versionFile.size() == kVersionFileSize
                     ? config.versionFile
                     : std::vector<std::uint8_t>(kVersionFileSize, 0);
    Blowfish cipher(kVersionKey);
    if (config.versionBlockAtOffset4) {
        std::fill(bytes.begin() + 4, bytes.begin() + 12, std::uint8_t{0});
        std::copy(text.begin(), text.end(), bytes.begin() + 4);
        cipher.encryptBlock(bytes.data() + 4, config.versionEndian);
    } else {
        std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
        std::copy(text.begin(), text.end(), bytes.begin());
        cipher.encryptBuffer(bytes, config.versionEndian);
    }
    return bytes;
}

} // namespace pk2

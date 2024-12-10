#include "utils.h"

#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <random>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#endif
#include <openssl/sha.h>
#include <nlohmann/json.hpp>

#include "world/chunk.h"
#include "networking/client.h"
#include "config.h"
#include "daft_hash.h"
#include "networking/fetch.h"
#include "server.h"
#include "zlib.h"
#include "entities/entity_factory.h"
#include "entities/item_entity.h"
#include "entities/player.h"
#include "utils/translation.h"

std::vector<uint8_t> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        logMessage("Failed to open icon file: " + filename, LOG_ERROR);
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    static constexpr char base64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    std::string encoded;
    uint8_t array3[3];
    uint8_t array4[4];

    for (size_t pos = 0; pos < data.size(); ) {
        size_t j = 0;
        while (j < 3 && pos < data.size()) {
            array3[j++] = data[pos++];
        }

        if (j == 0) {
            break;
        }

        for (; j < 3; j++) {
            array3[j] = 0;
        }

        array4[0] = (array3[0] & 0xfc) >> 2;
        array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
        array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
        array4[3] = array3[2] & 0x3f;

        for (size_t k = 0; k < 4; k++) {
            if (j + k > data.size() + 1) {
                encoded += '=';
            } else {
                encoded += base64Chars[array4[k]];
            }
        }
    }

    return encoded;
}

std::string getDirectoryFromPath(const std::string& filepath) {
    size_t pos = filepath.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    return filepath.substr(0, pos);
}

bool isAbsolutePath(const std::string& path) {
#ifdef _WIN32
    // On Windows, an absolute path starts with a drive letter or UNC path
    if (path.length() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    if (path.length() > 1 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    return false;
#else
    // On Unix-like systems, an absolute path starts with '/'
    return !path.empty() && path[0] == '/';
#endif
}

std::array<uint8_t, 16> generateUUID() {
    std::array<uint8_t, 16> uuid;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);

    for (auto& byte : uuid) {
        byte = dis(gen);
    }

    // Set version and variant bits according to RFC 4122
    uuid[6] = (uuid[6] & 0x0F) | 0x40; // Version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80; // Variant

    return uuid;
}

// Function to convert UUID bytes to a string
std::string uuidToString(const std::array<uint8_t, 16>& uuidBytes) {
    std::stringstream ss;
    ss << std::hex;
    for (size_t i = 0; i < uuidBytes.size(); ++i) {
        ss << std::setw(2) << std::setfill('0') << static_cast<int>(uuidBytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << "-";
        }
    }
    return ss.str();
}

nbt::tag_compound jsonToTagCompound(const nlohmann::json& j) {
    nbt::tag_compound compound;

    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        const nlohmann::json& value = it.value();

        if (value.is_boolean()) {
            compound[key] = nbt::tag_byte(value.get<bool>() ? 1 : 0);
        }
        else if (value.is_number_integer()) {
            compound[key] = nbt::tag_int(value.get<int>());
        }
        else if (value.is_number_unsigned()) {
            compound[key] = nbt::tag_int(static_cast<int>(value.get<unsigned int>()));
        }
        else if (value.is_number_float()) {
            compound[key] = nbt::tag_float(value.get<float>());
        }
        else if (value.is_string()) {
            compound[key] = nbt::tag_string(value.get<std::string>());
        }
        else if (value.is_object()) {
            compound[key] = jsonToTagCompound(value);
        }
        else if (value.is_array()) {
            // Determine the type of the first element to decide the TagList type
            if (!value.empty()) {
                const nlohmann::json& firstElem = value[0];
                if (firstElem.is_boolean()) {
                    nbt::tag_list tagList(nbt::tag_type::Byte);
                    for (const auto& elem : value) {
                        tagList.push_back(nbt::tag_byte(elem.get<bool>() ? 1 : 0));
                    }
                    compound[key] = std::move(tagList);
                }
                else if (firstElem.is_number_integer()) {
                    nbt::tag_list tagList(nbt::tag_type::Int);
                    for (const auto& elem : value) {
                        tagList.push_back(nbt::tag_int(elem.get<int>()));
                    }
                    compound[key] = std::move(tagList);
                }
                else if (firstElem.is_number_float()) {
                    nbt::tag_list tagList(nbt::tag_type::Float);
                    for (const auto& elem : value) {
                        tagList.push_back(nbt::tag_float(elem.get<float>()));
                    }
                    compound[key] = std::move(tagList);
                }
                else if (firstElem.is_string()) {
                    nbt::tag_list tagList(nbt::tag_type::String);
                    for (const auto& elem : value) {
                        tagList.push_back(nbt::tag_string(elem.get<std::string>()));
                    }
                    compound[key] = std::move(tagList);
                }
                else if (firstElem.is_object()) {
                    nbt::tag_list tagList(nbt::tag_type::Compound);
                    for (const auto& elem : value) {
                        tagList.push_back(jsonToTagCompound(elem));
                    }
                    compound[key] = std::move(tagList);
                }
                else {
                    logMessage("Unsupported JSON array element type for key: " + key, LOG_ERROR);
                }
            } else {
                // Empty array; default to TAG_END
                nbt::tag_list tagList(nbt::tag_type::End);
                compound[key] = std::move(tagList);
            }
        }
        else {
            logMessage("Unsupported JSON type for key: " + key, LOG_ERROR);
        }
    }

    return compound;
}

std::vector<uint8_t> serializeNBT(const nbt::tag& compound, bool excludeName, const std::string &name) {
    std::ostringstream os(std::ios::binary);

    if (excludeName) {
        // Serialize the compound without the type ID and name
        // The write_tag function typically writes: [Type ID][Name Length][Name][Payload]
        // For Protocol >=764, we need to omit the [Type ID][Name Length][Name]
        // We'll serialize normally and then strip the first two bytes

        nbt::io::write_tag("", compound, os, endian::big); // Write with empty name

        // Extract the data from the stream
        std::string serializedData = os.str();

        // Verify that the first byte is TAG_Compound (0x0a) and name length is 0x00
        if (serializedData.size() >= 2 &&
            static_cast<uint8_t>(serializedData[0]) == 0x0a &&
            static_cast<uint8_t>(serializedData[1]) == 0x00) {
            // Remove the first two bytes
            serializedData.erase(1, 2);
            }

        // Convert to byte vector
        std::vector<uint8_t> data(serializedData.begin(), serializedData.end());
        return data;
    }

    // Serialize normally (including type ID and name)
    nbt::io::write_tag(name, compound, os, endian::big);
    std::string serializedData = os.str();
    std::vector<uint8_t> data(serializedData.begin(), serializedData.end());
    return data;
}

int32_t generateUniqueTeleportID() {
    static std::atomic teleportIDCounter(1);
    return teleportIDCounter++;
}

int32_t getChunkCoordinate(double position) {
    if (position >= 0) {
        return static_cast<int32_t>(position) / CHUNK_WIDTH;
    }
    return (static_cast<int32_t>(position) - 15) / CHUNK_WIDTH;
}

std::string bytesToUUIDString(const std::array<uint8_t, 16>& uuidBytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < 16; ++i) {
        ss << std::setw(2) << static_cast<int>(uuidBytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << '-';
        }
    }
    return ss.str();
}

std::array<uint8_t, 16> stringUUIDToBytes(const std::string& uuid) {
    std::array<uint8_t, 16> uuidBytes;
    std::string uuidStr = uuid;
    if (uuidStr.size() == 36) {
        // Remove hyphens from UUID string
        std::erase(uuidStr, '-');
    }

    if (uuidStr.size() != 32) {
        logMessage("Invalid UUID string: " + uuid, LOG_ERROR);
        return uuidBytes;
    }

    for (size_t i = 0; i < 16; ++i) {
        std::string byteStr = uuidStr.substr(i * 2, 2);
        uuidBytes[i] = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
    }

    return uuidBytes;
}

std::string stripNamespace(const std::string& namespacedID) {
    size_t colonPos = namespacedID.find(':');
    if (colonPos != std::string::npos && colonPos + 1 < namespacedID.length()) {
        return namespacedID.substr(colonPos + 1);
    }
    return namespacedID; // Return original if no namespace found
}

uint64_t encodePosition(int32_t x, int32_t y, int32_t z) {
    return ((static_cast<uint64_t>(x & 0x3FFFFFF) << 38) |
            (static_cast<uint64_t>(z & 0x3FFFFFF) << 12) |
            (static_cast<uint64_t>(y & 0xFFF)));
}

void decodePosition(uint64_t val, int32_t &x, int32_t &y, int32_t &z) {
    x = static_cast<int32_t>(val >> 38);
    z = static_cast<int32_t>((val >> 12) & 0x3FFFFFF);
    y = static_cast<int32_t>(val & 0xFFF);

    // Handle sign extension
    if (x >= (1 << 25)) { x -= (1 << 26); }
    if (z >= (1 << 25)) { z -= (1 << 26); }
    if (y >= (1 << 11)) { y -= (1 << 12); }
}

std::vector<uint8_t> compressGZip(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        throw std::runtime_error("Input data is empty.");
    }

    // Initialize zlib's deflate stream
    z_stream strm = {};
    strm.next_in = const_cast<Bytef*>(data.data());
    strm.avail_in = data.size();

    // 15 + 16 to enable GZip encoding with maximum window size
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib for GZip compression.");
    }

    std::vector<uint8_t> compressedData;

    int ret;
    do {
        constexpr size_t bufferSize = 32768; // 32KB buffer
        uint8_t outBuffer[bufferSize];
        strm.next_out = outBuffer;
        strm.avail_out = bufferSize;

        ret = deflate(&strm, Z_FINISH); // Use Z_FINISH to indicate end of compression

        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&strm);
            throw std::runtime_error("Zlib stream error during compression.");
        }

        size_t have = bufferSize - strm.avail_out;
        compressedData.insert(compressedData.end(), outBuffer, outBuffer + have);
    } while (ret != Z_STREAM_END);

    deflateEnd(&strm);

    return compressedData;
}

std::vector<uint8_t> decompressGZip(const std::vector<uint8_t>& compressedData) {
    if (compressedData.empty()) {
        throw std::runtime_error("Compressed data is empty.");
    }

    // Initialize zlib's inflate stream
    z_stream strm = {};
    strm.next_in = const_cast<Bytef*>(compressedData.data());
    strm.avail_in = compressedData.size();

    // 16 + MAX_WBITS to enable gzip decoding with automatic header detection
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib for GZip decompression.");
    }

    // Buffer to hold decompressed data
    std::vector<uint8_t> decompressedData;

    int ret;
    do {
        constexpr size_t bufferSize = 32768; // 32KB buffer
        uint8_t outBuffer[bufferSize];
        strm.next_out = outBuffer;
        strm.avail_out = bufferSize;

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            inflateEnd(&strm);
            throw std::runtime_error("Zlib stream error during decompression.");
        }

        switch (ret) {
            case Z_NEED_DICT:
                ret = Z_DATA_ERROR;     // Fall through
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                inflateEnd(&strm);
            throw std::runtime_error("Zlib decompression error.");
            default: break;
        }

        size_t have = bufferSize - strm.avail_out;
        decompressedData.insert(decompressedData.end(), outBuffer, outBuffer + have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    return decompressedData;
}

std::vector<uint8_t> compressData(const std::vector<uint8_t>& data) {
    z_stream zs = {};

    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflateInit failed while compressing.");
    }

    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = data.size();

    int ret;
    std::vector<uint8_t> out;

    do {
        char outbuffer[32768];
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (out.size() < zs.total_out) {
            out.insert(out.end(), outbuffer, outbuffer + sizeof(outbuffer) - zs.avail_out);
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) { // an error occurred that was not EOF
        throw std::runtime_error("Exception during zlib compression.");
    }

    return out;
}

// Decompress data using zlib (inflate)
std::vector<uint8_t> decompressData(const std::vector<uint8_t>& data) {
    z_stream zs = {};

    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed while decompressing.");
    }

    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = data.size();

    int ret;
    std::vector<uint8_t> out;

    do {
        char outbuffer[32768];
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = inflate(&zs, 0);

        if (out.size() < zs.total_out) {
            out.insert(out.end(), outbuffer, outbuffer + sizeof(outbuffer) - zs.avail_out);
        }

    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) { // an error occurred that was not EOF
        throw std::runtime_error("Exception during zlib decompression.");
    }

    return out;
}

std::string computeServerHash(const std::string& serverId, const std::array<uint8_t, 16>& sharedSecret, const std::vector<uint8_t>& serverPublicKey) {
    minecraft::protocol::daft_hash_impl hasher;
    hasher.update(serverId);
    hasher.update(std::string_view(reinterpret_cast<const char*>(sharedSecret.data()), sharedSecret.size()));
    hasher.update(std::string_view(reinterpret_cast<const char*>(serverPublicKey.data()), serverPublicKey.size()));
    return hasher.finalise();
}

std::string getClientIPAddress(const ClientConnection& client) {
#ifdef _WIN32
    sockaddr_in addr;
    int addrLen = sizeof(addr);
    if (getpeername(client.socket, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        return "Unknown";
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip);
#else
    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(client.socket, reinterpret_cast<sockaddr*>(&addr), &addr_len) == -1) {
        return "Unknown";
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip);
#endif
}

std::string generateServerID() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 20; ++i) {
        ss << dis(gen);
    }
    return ss.str();
}

std::vector<uint8_t> base64Decode(const std::string &encoded) {
    int decodeLen = encoded.size() * 3 / 4;
    std::vector<uint8_t> buffer(decodeLen);

    BIO *bio = BIO_new_mem_buf(encoded.data(), encoded.size());
    if (!bio) {
        logMessage("Failed to create BIO memory buffer.", LOG_ERROR);
        return {};
    }

    BIO *b64 = BIO_new(BIO_f_base64());
    if (!b64) {
        BIO_free(bio);
        logMessage("Failed to create BIO base64 filter.", LOG_ERROR);
        return {};
    }

    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    int decodedLength = BIO_read(bio, buffer.data(), encoded.size());
    if (decodedLength < 0) {
        logMessage("Base64 decoding failed.", LOG_ERROR);
        BIO_free_all(bio);
        return {};
    }

    buffer.resize(decodedLength);
    BIO_free_all(bio);

    return buffer;
}

nbt::tag_compound createTextComponent(const std::string& text, const std::string& color) {
    nbt::tag_compound textComponent;
    textComponent["text"] = nbt::tag_string(text);
    textComponent["color"] = nbt::tag_string(color);
    return textComponent;
}

std::shared_ptr<Player> getPlayer(const std::string &username) {
    playersMutex.lock();
    std::shared_ptr<Player> player = globalPlayersName[username];
    playersMutex.unlock();
    if (player == nullptr) {
        return nullptr;
    }
    return player;
}

std::string capitalizeFirstLetter(const std::string& str) {
    if (str.empty()) return str; // Handle empty string

    std::string result = str;
    // Capitalize the first character
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));

    // Make the rest of the string lowercase for consistency
    std::transform(result.begin() + 1, result.end(), result.begin() + 1, [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });

    return result;
}

Gamemode stringToGamemode(const std::string& gamemode) {
    if (gamemode == "survival") {
        return SURVIVAL;
    }
    if (gamemode == "creative") {
        return CREATIVE;
    }
    if (gamemode == "adventure") {
        return ADVENTURE;
    }
    if (gamemode == "spectator") {
        return SPECTATOR;
    }
    return CREATIVE;
}

std::string gamemodeToString(Gamemode gamemode) {
    switch (gamemode) {
        case SURVIVAL:
            return "survival";
        case CREATIVE:
            return "creative";
        case ADVENTURE:
            return "adventure";
        case SPECTATOR:
            return "spectator";
        default:
            return "Unknown";
    }
}

void logMessage(const std::string& message, LogType logType) {
    if (logType == LOG_INFO) {
        std::cout << "[INFO] " << message << std::endl;
    } else if (logType == LOG_WARNING) {
        std::cout << "[WARNING] " << message << std::endl;
    } else if (logType == LOG_ERROR) {
        std::cerr << "[ERROR] " << message << std::endl;
    } else if (logType == LOG_DEBUG && PRINT_DEBUG) {
        std::cout << "[DEBUG] " << message << std::endl;
    } else if (logType == LOG_RAW) {
        std::cout << message << std::endl;
    }
}

std::string computeSHA1(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        logMessage("Failed to open file for hashing: " + filePath, LOG_ERROR);
        return "";
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        logMessage("EVP_MD_CTX_new failed", LOG_ERROR);
        return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha1(), nullptr) != 1) {
        logMessage("EVP_DigestInit_ex failed", LOG_ERROR);
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        if (EVP_DigestUpdate(mdctx, buffer, file.gcount()) != 1) {
            logMessage("EVP_DigestUpdate failed", LOG_ERROR);
            EVP_MD_CTX_free(mdctx);
            return "";
        }
    }

    // Handle the last chunk
    if (file.gcount() > 0) {
        if (EVP_DigestUpdate(mdctx, buffer, file.gcount()) != 1) {
            logMessage("EVP_DigestUpdate failed", LOG_ERROR);
            EVP_MD_CTX_free(mdctx);
            return "";
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        logMessage("EVP_DigestFinal_ex failed", LOG_ERROR);
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    EVP_MD_CTX_free(mdctx);

    // Convert hash to hex string
    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}

// Function to remove resource packs not present in the config
void cleanupResourcePacks() {
    std::string resourcePacksDir = "../resources/resource_packs";

    for (const auto& entry : std::filesystem::directory_iterator(resourcePacksDir)) {
        if (entry.is_regular_file()) {
            std::string filePath = entry.path().string();
            std::string fileName = entry.path().filename().string();
            // Extract UUID from filename
            size_t pos = fileName.find(".zip");
            if (pos == std::string::npos) continue;
            std::string uuid = fileName.substr(0, pos);

            // Check if UUID is present in config
            bool exists = false;
            for (const auto& pack : serverConfig.resourcePacks) {
                if (pack.uuid == uuid) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                // Delete the file
                try {
                    std::filesystem::remove(filePath);
                    logMessage("Removed outdated resource pack: " + filePath, LOG_INFO);
                } catch (const std::filesystem::filesystem_error& e) {
                    logMessage("Failed to remove resource pack: " + filePath + " Error: " + e.what(), LOG_ERROR);
                }
            }
        }
    }
}

void manageResourcePacks() {
    // Directory to store downloaded resource packs
    std::string resourcePacksDir = "../resources/resource_packs";
    std::filesystem::create_directories(resourcePacksDir);

    // Iterate through each resource pack in the config
    for (auto& pack : serverConfig.resourcePacks) {
        // Validate URL
        if (!validateResourcePackURL(pack)) {
            logMessage("Skipping resource pack due to invalid URL: " + pack.url, LOG_WARNING);
            continue;
        }

        // Define the download path
        std::string fileName = pack.uuid + ".zip"; // Naming convention: UUID.zip
        std::string downloadPath = resourcePacksDir + "/" + fileName;

        // Check if the file already exists
        if (std::filesystem::exists(downloadPath)) {
            logMessage("Resource pack already downloaded: " + downloadPath, LOG_DEBUG);
        } else {
            // Download the resource pack
            if (!downloadResourcePack(pack, downloadPath)) {
                logMessage("Failed to download resource pack: " + pack.url, LOG_ERROR);
                continue;
            }
        }

        // Compute hash if not provided
        if (pack.hash.empty()) {
            std::string computedHash = computeSHA1(downloadPath);
            if (computedHash.empty()) {
                logMessage("Failed to compute hash for resource pack: " + downloadPath, LOG_ERROR);
                continue;
            }
            pack.hash = computedHash;
            logMessage("Computed hash for resource pack " + pack.uuid + ": " + pack.hash, LOG_DEBUG);
        } else {
            // Verify existing hash matches the computed hash
            std::string existingHash = pack.hash;
            std::string computedHash = computeSHA1(downloadPath);
            if (computedHash.empty()) {
                logMessage("Failed to compute hash for resource pack: " + downloadPath, LOG_ERROR);
                continue;
            }
            if (existingHash != computedHash) {
                logMessage("Hash mismatch for resource pack: " + pack.url, LOG_ERROR);
                logMessage("Expected: " + existingHash + ", Computed: " + computedHash, LOG_ERROR);
                continue;
            }
            logMessage("Hash verification passed for resource pack: " + pack.url, LOG_DEBUG);
        }
    }

    // After ensuring all configured packs are downloaded, clean up outdated packs
    cleanupResourcePacks();
}

ParsedURL parseURL(const std::string& url) {
    ParsedURL result;
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::invalid_argument("Invalid URL: Missing protocol scheme (e.g., https://)");
    }

    result.protocol = url.substr(0, scheme_end);
    std::ranges::transform(result.protocol, result.protocol.begin(), ::tolower);
    size_t pos = scheme_end + 3; // Skip "://"

    // Find the end of the host
    size_t host_end = url.find_first_of(":/", pos);
    if (host_end == std::string::npos) {
        result.host = url.substr(pos);
        result.port = (result.protocol == "https") ? 443 : 80;
        result.path = "/";
        return result;
    }

    result.host = url.substr(pos, host_end - pos);

    // Check if port is specified
    if (url[host_end] == ':') {
        size_t port_end = url.find('/', host_end);
        std::string port_str;
        if (port_end == std::string::npos) {
            port_str = url.substr(host_end + 1);
            pos = url.length();
        } else {
            port_str = url.substr(host_end + 1, port_end - host_end - 1);
            pos = port_end;
        }

        try {
            result.port = std::stoi(port_str);
        } catch (...) {
            throw std::invalid_argument("Invalid URL: Port is not a number");
        }

    } else {
        // No port specified
        result.port = (result.protocol == "https") ? 443 : 80;
        pos = host_end;
    }

    // Extract path
    if (pos < url.length()) {
        result.path = url.substr(pos);
    } else {
        result.path = "/";
    }

    // Ensure path starts with '/'
    if (!result.path.empty() && result.path[0] != '/') {
        result.path = "/" + result.path;
    }

    return result;
}

// Utility function to convert sockaddr_in to a unique key
std::string sockaddrToString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

int32_t generateChallengeToken() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int32_t> dist(0, INT32_MAX); // 0 to 2,147,483,647
    return dist(rng);
}

std::string removeFormattingCodes(const std::string &input) {
    std::string result;
    result.reserve(input.size()); // Reserve space to optimize performance

    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\302') {
            // Skip the '§' character and the next character if it exists
            if (i + 1 < input.length()) {
                i += 2; // Skip the formatting code character
            }
            // If '§' is the last character, just skip it
        } else {
            result += input[i];
        }
    }

    return result;
}

std::string colorBossbarMessage(const Bossbar& bossbar) {
    switch (bossbar.getColor()) {
        case PINK:
            return "§c"; // For some reason minecraft uses red
        case BLUE:
            return "§9";
        case RED:
            return  "§4"; // Dark red
        case GREEN:
            return "§a"; // Light green
        case YELLOW:
            return "§e";
        case PURPLE:
            return "§1"; // For some reason minecraft uses dark blue
        case WHITE:
            return "§f";
    }
    return "";
}

BossbarColor stringToBossbarColor(const std::string& color) {
    if (color == "pink") {
        return PINK;
    }
    if (color == "blue") {
        return BLUE;
    }
    if (color == "red") {
        return RED;
    }
    if (color == "green") {
        return GREEN;
    }
    if (color == "yellow") {
        return YELLOW;
    }
    if (color == "purple") {
        return PURPLE;
    }
    if (color == "white") {
        return WHITE;
    }
    return WHITE;
}

BossbarDivision stringToBossbarDivision(const std::string& division) {
    if (division == "progress") {
        return NO_DIVISION;
    }
    if (division == "notched_6") {
        return SIX_NOTCHES;
    }
    if (division == "notched_10") {
        return TEN_NOTCHES;
    }
    if (division == "notched_12") {
        return TWELVE_NOTCHES;
    }
    if (division == "notched_20") {
        return TWENTY_NOTCHES;
    }
    return NO_DIVISION;
}

int64_t parseDuration(const std::string& durationStr) {
    int64_t ticks = 0;
    std::string durationStrCopy = durationStr;
    try {
        // Handle unit suffixes: d, s, t
        size_t len = durationStrCopy.length();
        char unit = 't'; // default unit
        if (len > 0 && (durationStrCopy[len - 1] == 'd' || durationStrCopy[len - 1] == 's' || durationStrCopy[len - 1] == 't')) {
            unit = durationStrCopy[len - 1];
            durationStrCopy = durationStrCopy.substr(0, len - 1);
        }

        float timeValue = std::stof(durationStrCopy);
        switch (unit) {
            case 'd':
                ticks = static_cast<int>(timeValue * 24000);
            break;
            case 's':
                ticks = static_cast<int>(timeValue * 20);
            break;
            case 't':
            default:
                ticks = static_cast<int>(timeValue);
            break;
        }
    }
    catch (const std::exception& e) {
        return 0;
    }

    return ticks;
}

std::vector<std::shared_ptr<Item>> getItemsFromBlock(int16_t blockstate) {
    std::vector<std::shared_ptr<Item>> items;
    for (const auto& block : blocks) {
        if (blockstate >= block.second.minStateId && blockstate <= block.second.maxStateId) {
            for (auto itemId : block.second.drops) {
                auto item = EntityFactory::createItem();
                item->setItemId(static_cast<int16_t>(itemId));
                item->setItemCount(1);
                items.push_back(item);
            }
            break;
        }
    }
    return items;
}

std::string getBlockName(int16_t blockstate) {
    for (const auto& block : blocks) {
        if (blockstate >= block.second.minStateId && blockstate <= block.second.maxStateId) {
            return block.first;
        }
    }
    return "";
}

double getRandomDouble(double min, double max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(min, max);
    return dis(gen);
}

int32_t posToBlockCoord(double pos) {
    return static_cast<int32_t>(std::floor(pos));
}

bool checkCollision(Item& item, BoundingBox& collidedBlockBox, Axis axis) {
    BoundingBox itemBox = item.getHitBox();

    // Determine the blocks overlapped by the item's bounding box
    int32_t minBlockX = posToBlockCoord((axis == Axis::X || axis == Axis::Y) ? itemBox.minX : item.getPositionX());
    int32_t maxBlockX = posToBlockCoord((axis == Axis::X || axis == Axis::Y) ? itemBox.maxX : item.getPositionX());
    int32_t minBlockY = posToBlockCoord(itemBox.minY);
    int32_t maxBlockY = posToBlockCoord(itemBox.maxY);
    int32_t minBlockZ = posToBlockCoord((axis == Axis::Y || axis == Axis::Z) ? itemBox.minZ : item.getPositionZ());
    int32_t maxBlockZ = posToBlockCoord((axis == Axis::Y || axis == Axis::Z) ? itemBox.maxZ : item.getPositionZ());

    for (int32_t x = minBlockX; x <= maxBlockX; ++x) {
        for (int32_t y = minBlockY; y <= maxBlockY; ++y) {
            for (int32_t z = minBlockZ; z <= maxBlockZ; ++z) {
                auto chunk = getChunkContainingBlock(x, y, z);
                auto blockstate = chunk->getBlock(getLocalCoordinate(x), y, getLocalCoordinate(z)).blockStateID;
                if (blockstate == blocks["air"].defaultState) {
                    continue; // No collision with air
                }

                // Get collision shapes for this block type
                // TODO: Get correct shapeId by using the blockstate and not get all of the shapes
                auto shapeIDs = blockNameToShapeIDs[getBlockName(blockstate)];
                std::vector<BoundingBox> shapesOpt;
                for (const auto& shapeID : shapeIDs) {
                    for (auto shape : shapeIDToShapes[static_cast<int>(shapeID)]) {
                        shapesOpt.emplace_back(shape);
                    }
                }
                for (const auto& shape : shapesOpt) {
                    // Convert block shape to world coordinates
                    BoundingBox blockBox{
                        static_cast<double>(x) + shape.minX,
                        static_cast<double>(y) + shape.minY,
                        static_cast<double>(z) + shape.minZ,
                        static_cast<double>(x) + shape.maxX,
                        static_cast<double>(y) + shape.maxY,
                        static_cast<double>(z) + shape.maxZ
                    };

                    if (itemBox.intersects(blockBox)) {
                        collidedBlockBox = blockBox;
                        return true; // Collision detected
                    }
                }
            }
        }
    }

    return false; // No collision
}

double calculateFinalVelocity(double initialVelocity, double drag, double acceleration, int ticksPassed, DragApplicationOrder order) {
    if (drag == 0.0) { // Avoid division by zero
        return initialVelocity + acceleration * ticksPassed;
    }

    double dragFactor = std::pow(1.0 - drag, ticksPassed);
    double accelerationFactor = (1.0 - dragFactor) / drag;

    if (order == BeforeAcceleration) {
        return (initialVelocity * dragFactor) - (acceleration * accelerationFactor);
    }
    // AfterAcceleration
    return (initialVelocity * dragFactor) - (acceleration * accelerationFactor * (1.0 - drag));
}

DiggingInfo calculateDiggingSpeed(int16_t blockstate, const std::shared_ptr<Player>& player) {
    DiggingInfo info;
    bool canHarvest = false;
    double speedMultiplier = 1.0;

    const int heldItemID = player->getHeldItemID();
    std::string toolName = "";
    if (heldItemID >= 0 && heldItemID < static_cast<int>(itemIDs.size())) {
        toolName = itemIDs[heldItemID].name;
    }

    // Find the target block based on blockstate
    const BlockData* targetBlock = nullptr;
    for (const auto& blockPair : blocks) {
        const BlockData& block = blockPair.second;
        if (blockstate >= block.minStateId && blockstate <= block.maxStateId) {
            targetBlock = &block;
            break;
        }
    }

    if (!targetBlock) {
        // Block not found
        return {false, -1.0, 0};
    }

    if (!targetBlock->diggable) {
        // Block cannot be broken (e.g., Bedrock)
        return {false, -1.0, 0};
    }

    // Check if the player's held tool is the best tool for the block
    bool isBestTool = false;
    if (targetBlock->harvestTools.empty()) {
        canHarvest = true;
        // TODO: Replace with tags
        if (targetBlock->material.find("mineable/shovel") != std::string::npos) {
            if (toolName.find("shovel") != std::string::npos) {
                isBestTool = true;
            }
        } else if (targetBlock->material.find("mineable/axe") != std::string::npos) {
            if (toolName.find("axe") != std::string::npos) {
                isBestTool = true;
            }
        } else if (targetBlock->material.find("mineable/pickaxe") != std::string::npos) {
            if (toolName.find("pickaxe") != std::string::npos) {
                isBestTool = true;
            }
        }
    }
    for (const auto& toolID : targetBlock->harvestTools) {
        if (toolID == heldItemID) {
            isBestTool = true;
            canHarvest = true;
            break;
        }
    }

    // Initialize speedMultiplier based on whether the tool is best for the block
    if (isBestTool) {
        speedMultiplier = 1.0; // Will be multiplied by tool speed
        // TODO: Apply Efficiency enchantment
        /*
        int efficiencyLevel = player->getEfficiencyLevel();
        if (efficiencyLevel > 0 && isBestTool) {
            speedMultiplier += static_cast<double>(efficiencyLevel * efficiencyLevel + 1);
        }
        */
    } else {
        // If the tool cannot harvest the block, set speedMultiplier based on canHarvest
        speedMultiplier = 1.0;
    }

    // Determine the tool's speed multiplier
    double toolSpeedMultiplier = 1.0;
    // TODO: Replace with tags
    if (toolName.find("shears") != std::string::npos) {
        // Special handling for shears
        // TODO: Adjust toolSpeedMultiplier based on specific block types (e.g., wool, cobwebs)
        toolSpeedMultiplier = 2.0; // Default for shears
    }
    else if (toolName.find("sword") != std::string::npos || toolName.find("mace") != std::string::npos) {
        toolSpeedMultiplier = 1.5;
    }
    else if (toolName.find("wood") != std::string::npos) {
        toolSpeedMultiplier = 2.0;
    }
    else if (toolName.find("stone") != std::string::npos) {
        toolSpeedMultiplier = 4.0;
    }
    else if (toolName.find("iron") != std::string::npos) {
        toolSpeedMultiplier = 6.0;
    }
    else if (toolName.find("diamond") != std::string::npos) {
        toolSpeedMultiplier = 8.0;
    }
    else if (toolName.find("netherite") != std::string::npos) {
        toolSpeedMultiplier = 9.0;
    }
    else if (toolName.find("gold") != std::string::npos) {
        toolSpeedMultiplier = 12.0;
    }
    else {
        toolSpeedMultiplier = 1.0; // No tool or unrecognized tool
    }

    if (isBestTool) {
        speedMultiplier *= toolSpeedMultiplier;
    }

    // TODO: Apply Haste or Conduit Power
    /*
    int hasteLevel = player->getHasteLevel();
    int conduitPowerLevel = player->getConduitPowerLevel();
    if (hasteLevel > 0 || conduitPowerLevel > 0) {
        int maxHasteConduit = std::max(hasteLevel, conduitPowerLevel);
        speedMultiplier *= (0.2 * static_cast<double>(maxHasteConduit) + 1.0);
    }
    */

    // TODO: Apply Mining Fatigue
    /*
    int miningFatigueLevel = player->getMiningFatigueLevel();
    if (miningFatigueLevel > 0) {
        // speedMultiplier *= 0.3 ^ min(level, 4)
        int effectiveLevel = std::min(miningFatigueLevel, 4);
        speedMultiplier *= std::pow(0.3, static_cast<double>(effectiveLevel));

    }
    */

    // TODO: Apply underwater penalty
    /*
    if (player->isInWater() && !player->hasAquaAffinity()) {
        speedMultiplier /= 5.0;
    }
    */

    // Apply onGround penalty
    if (!player->isOnGround()) {
        speedMultiplier /= 5.0;
    }

    // Calculate damage
    double damage = speedMultiplier / targetBlock->hardness;
    if (canHarvest) {
        damage /= 30.0;
    } else {
        damage /= 100.0;
    }

    // Instant breaking condition
    if (damage > 1.0) {
        return {canHarvest, 0.0}; // Instant break
    }

    // Calculate the number of ticks (round up)
    double ticks = std::ceil(1.0 / damage);

    // Convert ticks to seconds (20 ticks = 1 second)
    double seconds = ticks / 20.0;

    // Ensure the time is a multiple of 1/20th of a second
    seconds = std::ceil(seconds * 20.0) / 20.0;

    info.canHarvest = canHarvest;
    info.diggingTime = seconds;
    info.speed = speedMultiplier;

    return info;
}

std::vector<std::string> splitString(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::stringstream ss(str);

    while (std::getline(ss, token, delimiter)) {
        // Optional: Trim whitespace from token if necessary
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    return tokens;
}
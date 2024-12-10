#ifndef UTILS_H
#define UTILS_H

#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_string.h>
#include <nlohmann/json_fwd.hpp>

#include "enums/enums.h"
#include "networking/fetch.h"

enum class Axis;
struct BoundingBox;
class Item;
class Bossbar;
struct sockaddr_in;
struct ClientConnection;
struct Player;

struct PublicKey {
    long expiresAt;
    std::vector<uint8_t> pubKey;
    std::vector<uint8_t> keySig;
};

struct MetadataEntry {
    uint8_t index;           // Unique index key
    uint32_t type;           // VarInt Enum representing the type
    std::vector<uint8_t> value; // Serialized value based on the type
};

struct ChatType {
    std::string identifier;
    std::string translation_key;
    std::vector<std::string> parameters;

    nbt::tag_compound serialize() const {
        nbt::tag_compound compound;

        // Serialize 'chat' component
        nbt::tag_compound chat;
        chat["translation_key"] = nbt::tag_string(translation_key);

        // Add parameters as a list
        nbt::tag_list withList(nbt::tag_type::String);
        for (const auto& param : parameters) {
            withList.push_back(nbt::tag_string(param));
        }
        chat["parameters"] = std::move(withList);
        compound["chat"] = std::move(chat);

        // Serialize 'narration' component
        nbt::tag_compound narration;
        narration["translation_key"] = nbt::tag_string("narration." + translation_key);
        nbt::tag_list narrationWithList(nbt::tag_type::String);
        for (const auto& param : parameters) {
            narrationWithList.push_back(nbt::tag_string(param));
        }
        narration["parameters"] = std::move(narrationWithList);
        compound["narration"] = std::move(narration);

        return compound;
    }
};


// Hash function for std::array<uint8_t, 16>
struct ArrayHash {
    std::size_t operator()(const std::array<uint8_t, 16>& arr) const {
        std::size_t hash = 0;
        for (auto byte : arr) {
            hash ^= std::hash<uint8_t>{}(byte) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// Equality function for std::array<uint8_t, 16>
struct ArrayEqual {
    bool operator()(const std::array<uint8_t, 16>& lhs, const std::array<uint8_t, 16>& rhs) const {
        return lhs == rhs;
    }
};

struct DiggingInfo {
    bool canHarvest;
    double diggingTime; // in seconds
    double speed;
};

std::vector<uint8_t> readFile(const std::string& filename);
std::string base64Encode(const std::vector<uint8_t>& data);
std::string getDirectoryFromPath(const std::string& filepath);
bool isAbsolutePath(const std::string& path);
std::array<uint8_t, 16> generateUUID();
std::string uuidToString(const std::array<uint8_t, 16>& uuidBytes);
nbt::tag_compound jsonToTagCompound(const nlohmann::json& j);
std::vector<uint8_t> serializeNBT(const nbt::tag& compound, bool excludeName, const std::string &name = "");
int32_t generateUniqueTeleportID();
int32_t getChunkCoordinate(double position);
std::string bytesToUUIDString(const std::array<uint8_t, 16>& uuidBytes);
std::array<uint8_t, 16> stringUUIDToBytes(const std::string& uuid);
std::string stripNamespace(const std::string& namespacedID);
uint64_t encodePosition(int32_t x, int32_t y, int32_t z);
void decodePosition(uint64_t val, int32_t &x, int32_t &y, int32_t &z);
std::vector<uint8_t> compressGZip(const std::vector<uint8_t>& data);
std::vector<uint8_t> decompressGZip(const std::vector<uint8_t>& compressedData);
std::vector<uint8_t> compressData(const std::vector<uint8_t>& data);
std::vector<uint8_t> decompressData(const std::vector<uint8_t>& data);
std::string computeServerHash(const std::string& serverId, const std::array<uint8_t, 16>& sharedSecret, const std::vector<uint8_t>& serverPublicKey);
std::string getClientIPAddress(const ClientConnection& client);
std::string generateServerID();
nbt::tag_compound createTextComponent(const std::string& text, const std::string& color = "white");
std::vector<uint8_t> base64Decode(const std::string &encoded);

std::shared_ptr<Player> getPlayer(const std::string &username);
std::string capitalizeFirstLetter(const std::string& str);
Gamemode stringToGamemode(const std::string& gamemode);
std::string gamemodeToString(Gamemode gamemode);
void logMessage(const std::string& message, LogType logType);
std::string computeSHA1(const std::string& filePath);
void manageResourcePacks();
ParsedURL parseURL(const std::string& url);
std::string sockaddrToString(const sockaddr_in& addr);
int32_t generateChallengeToken();
std::string removeFormattingCodes(const std::string &input);
std::string colorBossbarMessage(const Bossbar& bossbar);
BossbarColor stringToBossbarColor(const std::string& color);
BossbarDivision stringToBossbarDivision(const std::string& division);
int64_t parseDuration(const std::string& durationStr);
std::vector<std::shared_ptr<Item>> getItemsFromBlock(int16_t blockstate);
double getRandomDouble(double min, double max);
bool checkCollision(Item& item, BoundingBox& collidedBlockBox, Axis axis);
double calculateFinalVelocity(double initialVelocity, double drag, double acceleration, int ticksPassed, DragApplicationOrder order);
DiggingInfo calculateDiggingSpeed(int16_t blockstate, const std::shared_ptr<Player>& player);
std::vector<std::string> splitString(const std::string& str, char delimiter);

#endif // UTILS_H

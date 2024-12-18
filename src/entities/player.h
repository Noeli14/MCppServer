#ifndef PLAYER_H
#define PLAYER_H
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "world/chunk.h"
#include "networking/client.h"
#include "entity.h"
#include "core/utils.h"
#include "inventories/player_inventory.h"

struct Player : Entity {
    std::array<uint8_t, 16> uuid; // UUID as bytes
    std::string uuidString; // UUID as string
    std::string name;
    std::unordered_map<std::string, std::pair<std::string, std::string>> properties; // property name -> (value, signature)
    Gamemode gameMode;
    bool listed; // Whether the player is listed on the player list
    int32_t ping; // Ping in milliseconds
    int32_t currentChunkX;
    int32_t currentChunkZ;
    uint8_t flags; // Bitfield for player states
    ClientConnection* client;
    std::unordered_set<ChunkCoordinates> currentViewedChunks;
    std::unordered_set<ChunkCoordinates> loadedChunks;
    int viewDistance;
    uint8_t activeSlot = 0;
    std::shared_ptr<PlayerInventory> inventory;
    std::shared_ptr<Inventory> currentInventory;
    std::vector<uint8_t> sessionId;
    PublicKey sessionKey;
    EVP_PKEY* publicKey = nullptr;
    std::string brand;
    std::string lang;
    uint8_t windowID = 0;

    // Mutex for thread-safe access to mining progress
    std::mutex miningMutex;
    // Current mining progress mapped by block position
    std::unordered_map<Position, MiningProgress, PositionHash> currentMining;

    void setSneaking(bool isSneaking) {
        if (isSneaking) {
            flags |= 0x02; // Set the sneaking bit
            hitBox = {-0.3, 0, -0.3, 0.3, 1.65, 0.3};
        } else {
            flags &= ~0x02; // Clear the sneaking bit
            hitBox = {-0.3, 0, -0.3, 0.3, 1.8, 0.3};
        }
    }

    bool isSneaking() const {
        return flags & 0x02;
    }

    bool operator==(const std::shared_ptr<Player> & shared) const;

    Player(const std::array<uint8_t, 16>& uuidBytes, const std::string& playerName, EntityType entityType = EntityType::Player);

    void serializeAdditionalData(std::vector<uint8_t> &packetData) const override;
    BoundingBox getPickUpBox() const;

    int8_t canItemBeAddedToInventory(uint16_t id, uint8_t count) const;
    void addItemToInventory(uint16_t id, uint8_t count);
    void setClient(ClientConnection* newClient) {
        client = newClient;
        inventory->client = newClient;
    }

    uint16_t getHeldItemID() const {
        if (currentInventory->slots.contains(activeSlot + 36) && currentInventory->slots.at(activeSlot + 36).itemId.has_value()) {
            return currentInventory->slots.at(activeSlot + 36).itemId.value();
        }
        return 0;
    }
};

#endif //PLAYER_H

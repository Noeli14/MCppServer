#include "client.h"
#include "network.h"
#include "core/utils.h"
#include "core/config.h"
#include <iostream>
#include <atomic>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "world/chunk.h"
#include "clientbound_packets.h"
#include "commands/CommandBuilder.h"
#include "registries/dimension_type.h"
#include "enums/enums.h"
#include "fetch.h"
#include "packet_ids.h"
#include "entities/player.h"
#include "registries/registry_manager.h"
#include "core/server.h"
#include "entities/entity_factory.h"
#include "entities/item_entity.h"
#include "entities/slot_data.h"
#include "inventories/crafting_table_inventory.h"
#include "utils/translation.h"

// TODO: Make sure EntityManager and connectedClients are thread-safe

// Global player count
std::atomic<int> playerCount(0);

// Function to disconnect client
void disconnectClient(const std::shared_ptr<Player>& player, const std::string& reason, bool disconnectPacket) {
    if (disconnectPacket) {
        sendDisconnectionPacket(*player->client, reason);
        logMessage("Player " + player->name + " disconnected. Reason: " + reason, LOG_INFO);
        // Close the socket
#ifdef _WIN32
        closesocket(player->client->socket);
#else
        close(player->client->socket);
#endif
    }
    player->client->connectionClosed = true;
    playersMutex.lock();
    globalPlayersName.erase(player->name);
    globalPlayers.erase(player->uuidString);
    playersMutex.unlock();
    --playerCount;
    connectedClientsMutex.lock();
    connectedClients.erase(player->uuidString);
    connectedClientsMutex.unlock();

    sendPlayerInfoRemove(player);

    std::lock_guard lock(chunkMapMutex);
    for (auto it = chunkViewersMap.begin(); it != chunkViewersMap.end(); )
    {
        // Remove the player from the vector
        auto& viewers = it->second;
        std::erase_if(viewers,
                      [&player](const std::shared_ptr<Player>& p) { return p == player; });

        // If the vector is empty, erase the map entry
        if (viewers.empty())
        {
            it = chunkViewersMap.erase(it); // Erase and advance iterator
        }
        else
        {
            ++it; // Just advance the iterator
        }
    }

    sendTranslatedChatMessage("multiplayer.player.left", false, "yellow", nullptr, true, player->name);
    entityManager.removeEntity(player->uuidString);
}

void handleStatusRequest(ClientConnection& client) {
    // Read Status Request packet
    std::vector<uint8_t> packetData;
    // Read and respond to Ping packet
    if (readUnencryptedPacket(client, packetData)) {
        size_t index = 0;
        int32_t packetID = parseVarInt(packetData, index);
        if (packetID == STATUS_REQUEST) {
            // Build JSON response using serverConfig
            nlohmann::json responseJson = {
                {"version", {{"name", serverConfig.server_version}, {"protocol", serverConfig.protocol_version}}},
                {"players", {{"max", serverConfig.maxPlayers}, {"online", playerCount.load()}, {"sample", nlohmann::json::array()}}},
                {"description", {{"text", serverConfig.motd}}}
            };

            // Add favicon if available
            std::vector<uint8_t> faviconData = readFile(serverConfig.icon);
            if (!faviconData.empty()) {
                std::string faviconBase64 = base64Encode(faviconData);
                responseJson["favicon"] = "data:image/png;base64," + faviconBase64;
            }

            std::string jsonResponse = responseJson.dump();

            // Build response packet
            std::vector<uint8_t> responseData;
            responseData.push_back(STATUS_RESPONSE);
            writeVarInt(responseData, static_cast<int32_t>(jsonResponse.size()));
            responseData.insert(responseData.end(), jsonResponse.begin(), jsonResponse.end());

            // Send response
            sendUnencryptedPacket(client, responseData);

            if (readPacketTimeout(client, packetData, 5000, true)) {
                // Read Ping packet
                index = 0;
                packetID = parseVarInt(packetData, index);
                if (packetID == PING_REQUEST) {
                    // Ping packet
                    std::vector<uint8_t> pongData;
                    pongData.push_back(PONG_RESPONSE);
                    pongData.insert(pongData.end(), packetData.begin() + index, packetData.end());
                    sendUnencryptedPacket(client, pongData);
                }
            }
        } else if (packetID == PING_REQUEST) {
            // Ping packet
            std::vector<uint8_t> pongData;
            pongData.push_back(PONG_RESPONSE);
            pongData.insert(pongData.end(), packetData.begin() + index, packetData.end());
            sendUnencryptedPacket(client, pongData);
        }
    }
}

void startKeepAliveLoop(ClientConnection& client) {
    // Send Keep Alive packets every 15 seconds
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (client.connectionClosed || !sendKeepAlivePacket(client)) {
            break;
        }
    }
}

void handleKeepAliveResponse(SocketType clientSock, const std::vector<uint8_t>& packetData, size_t index) {
    // Read the Keep Alive ID (Long)
    if (index + 8 > packetData.size()) {
        logMessage("Invalid Keep Alive packet received", LOG_WARNING);
        return;
    }

    uint64_t keepAliveID = 0;
    for (int i = 0; i < 8; ++i) {
        keepAliveID = (keepAliveID << 8) | packetData[index + i];
    }
    logMessage("Received Keep Alive Response with ID: " + std::to_string(keepAliveID), LOG_DEBUG);

    // TODO: verify that the keepAliveID matches the one sent
}

void handleTeleportConfirm(ClientConnection& client, const std::vector<uint8_t>& packetData, size_t index, int teleportID) {
    client.state = ClientState::Play;
    // For now we don't need to do anything else
}

bool handleClientInformation(Player& player, const std::vector<uint8_t> & packetData, size_t index) {
    // Read Locale (String)
    std::string locale = parseString(packetData, index);
    player.lang = locale;
    // Read View Distance (Byte)
    uint8_t viewDistance = packetData[index++];
    player.viewDistance = viewDistance;
    // Read Chat Mode (VarInt)
    int32_t chatMode = parseVarInt(packetData, index);
    // Read Chat Colors (Boolean)
    bool chatColors = packetData[index++] != 0;
    // Read Displayed Skin Parts (Unsigned Byte)
    uint8_t displayedSkinParts = packetData[index++];
    // Read Main Hand (VarInt)
    int32_t mainHand = parseVarInt(packetData, index);
    // Read Enable Text Filtering (Boolean)
    bool enableTextFiltering = packetData[index++] != 0;
    // Read Allow Server Listings (Boolean)
    bool allowServerListings = packetData[index++] != 0;

    return true;
}

bool handleZeroPacket(ClientConnection& client, const std::vector<uint8_t>& packetData, size_t index, const std::shared_ptr<Player>& player) {
    // Attempt to parse the teleport ID first
    size_t unchangedIndex = index;
    int32_t teleportID = parseVarInt(packetData, index);

    bool isTeleportConfirm = false;
    {
        std::lock_guard lock(client.mutex);
        if (client.pendingTeleportIDs.contains(teleportID)) {
            isTeleportConfirm = true;
            client.pendingTeleportIDs.erase(teleportID);
        }
    }

    if (isTeleportConfirm && client.state == ClientState::AwaitingTeleportConfirm) {
        handleTeleportConfirm(client, packetData, index, teleportID);
        return false;
    }
    return handleClientInformation(*player, packetData, unchangedIndex);
}

void handleClientSettings(SocketType clientSock, const std::vector<uint8_t>& packetData, size_t index) {

}

void handlePlayerOnGround(SocketType clientSock, const std::vector<uint8_t>& packetData, size_t index, const std::shared_ptr<Player>& player) {
    bool onGround = packetData[index++] != 0;
    player->onGround = onGround;
}

void handlePlayerRotation(SocketType clientSock, const std::vector<uint8_t>& packetData, size_t index, const std::shared_ptr<Player>& player) {
    // Read Yaw (Float)
    float yaw = parseFloat(packetData, index);
    // Read Pitch (Float)
    float pitch = parseFloat(packetData, index);
    // Read On Ground (Boolean)
    bool onGround = packetData[index++] != 0;

    // Update player state
    player->rotation.yaw = yaw;
    player->rotation.pitch = pitch;
    player->rotation.headYaw = yaw;
    player->onGround = onGround;

    sendEntityRotationPacket(player);
    sendHeadRotationPacket(player);
}

void handlePlayerPositionAndRotationPacket(ClientConnection& client, const std::vector<uint8_t> & vector, size_t size, const std::shared_ptr<Player>& player) {
    double x = parseDouble(vector, size);
    double feetY = parseDouble(vector, size);
    double z = parseDouble(vector, size);
    float yaw = parseFloat(vector, size);
    float pitch = parseFloat(vector, size);
    bool onGround = vector[size++] != 0;

    // Calculate new chunk coordinates
    int32_t newChunkX = getChunkCoordinate(x);
    int32_t newChunkZ = getChunkCoordinate(z);

    // Check if chunk coordinates have changed
    if (newChunkX != player->currentChunkX || newChunkZ != player->currentChunkZ) {
        int32_t oldChunkX = player->currentChunkX;
        int32_t oldChunkZ = player->currentChunkZ;

        player->currentChunkX = newChunkX;
        player->currentChunkZ = newChunkZ;

        // Send Set Center Chunk packet to the client
        sendSetCenterChunkPacket(client, newChunkX, newChunkZ);

        // Update chunk viewers
        updatePlayerChunkView(player, oldChunkX, oldChunkZ, newChunkX, newChunkZ);

        std::vector<ChunkCoordinates> chunksToLoad = getChunksInView(newChunkX, newChunkZ, std::min(player->viewDistance, serverConfig.viewDistance));

        // Remove chunks that are already loaded
        for (auto it = chunksToLoad.begin(); it != chunksToLoad.end();) {
            if (player->loadedChunks.contains(*it)) {
                it = chunksToLoad.erase(it);
            } else {
                ++it;
            }
        }

        for(auto chunkCoord : chunksToLoad) {
            player->loadedChunks.insert(chunkCoord);
            // TODO: Remove unloaded chunks
            if (auto chunk = getOrLoadChunk(chunkCoord.chunkX, chunkCoord.chunkZ)) {
                sendChunkDataToPlayer(client, chunk);
            }
        }
    }

    // Calculate deltas
    double deltaX = x - player->position.x;
    double deltaY = feetY - player->position.y;
    double deltaZ = z - player->position.z;
    auto deltaXFixed = static_cast<short>(x * 4096 - player->position.x * 4096);
    auto deltaYFixed = static_cast<short>(feetY * 4096 - player->position.y * 4096);
    auto deltaZFixed = static_cast<short>(z * 4096 - player->position.z * 4096);

    // Update server state
    player->position.x = x;
    player->position.y = feetY;
    player->position.z = z;
    player->rotation.yaw = yaw;
    player->rotation.pitch = pitch;
    player->rotation.headYaw = yaw;
    player->onGround = onGround;

    // Decide which packet to send based on movement magnitude
    if (std::abs(deltaX) < 7.999755859375 && std::abs(deltaY) < 7.999755859375 && std::abs(deltaZ) < 7.999755859375) {
        // Small movement: Use Relative Move
        sendEntityLookAndRelativeMovePacket(player, deltaXFixed, deltaYFixed, deltaZFixed, yaw, pitch);
    } else {
        // Large movement: Use Teleport
        sendEntityTeleportPacket(player);
    }
    sendHeadRotationPacket(player);

    for (const auto &val: entityManager.getAllEntities() | std::views::values) {
        if (val->type == EntityType::Item) {
            auto item = std::static_pointer_cast<Item>(val);
            BoundingBox playerBox = player->getPickUpBox();
            BoundingBox itemBox = item->getHitBox();

            if (item->getCooldown() == 0 && playerBox.intersects(itemBox)) {
                const uint8_t itemsToAdd = player->canItemBeAddedToInventory(item->id(), item->getCount());
                if (itemsToAdd > 0) {
                    sendPickUpItem(item, player, itemsToAdd);
                    entityManager.removeEntity(item->uuidString);
                    player->addItemToInventory(item->id(), itemsToAdd);
                    break;
                }
            }
        }
    }
}

void handlePlayerPosition(ClientConnection& client, const std::vector<uint8_t>& vector, size_t& index, const std::shared_ptr<Player>& player) {
    double x = parseDouble(vector, index);
    double feetY = parseDouble(vector, index);
    double z = parseDouble(vector, index);
    bool onGround = vector[index++] != 0;

    // Calculate new chunk coordinates
    int32_t newChunkX = getChunkCoordinate(x);
    int32_t newChunkZ = getChunkCoordinate(z);

    // Check if chunk coordinates have changed
    if (newChunkX != player->currentChunkX || newChunkZ != player->currentChunkZ) {
        int32_t oldChunkX = player->currentChunkX;
        int32_t oldChunkZ = player->currentChunkZ;

        player->currentChunkX = newChunkX;
        player->currentChunkZ = newChunkZ;

        // Send Set Center Chunk packet to the client
        sendSetCenterChunkPacket(client, newChunkX, newChunkZ);

        // Update chunk viewers
        updatePlayerChunkView(player, oldChunkX, oldChunkZ, newChunkX, newChunkZ);

        std::vector<ChunkCoordinates> chunksToLoad = getChunksInView(newChunkX, newChunkZ, std::min(player->viewDistance, serverConfig.viewDistance));

        // Remove chunks that are already loaded
        for (auto it = chunksToLoad.begin(); it != chunksToLoad.end();) {
            if (player->loadedChunks.contains(*it)) {
                it = chunksToLoad.erase(it);
            } else {
                ++it;
            }
        }

        for(auto chunkCoord : chunksToLoad) {
            auto chunk = getOrLoadChunk(chunkCoord.chunkX, chunkCoord.chunkZ);
            player->loadedChunks.insert(chunkCoord);
            if (chunk) {
                sendChunkDataToPlayer(client, chunk);
            }
        }
    }

    // Calculate deltas
    double deltaX = x - player->position.x;
    double deltaY = feetY - player->position.y;
    double deltaZ = z - player->position.z;
    auto deltaXFixed = static_cast<short>(x * 4096 - player->position.x * 4096);
    auto deltaYFixed = static_cast<short>(feetY * 4096 - player->position.y * 4096);
    auto deltaZFixed = static_cast<short>(z * 4096 - player->position.z * 4096);

    // Update server state
    player->position.x = x;
    player->position.y = feetY;
    player->position.z = z;
    player->onGround = onGround;

    // Decide which packet to send based on movement magnitude
    if (std::abs(deltaX) < 7.999755859375 && std::abs(deltaY) < 7.999755859375 && std::abs(deltaZ) < 7.999755859375) {
        // Small movement: Use Relative Move
        sendPlayerRelativeMovePacket(player, deltaXFixed, deltaYFixed, deltaZFixed);
    } else {
        // Large movement: Use Teleport
        sendEntityTeleportPacket(player);
    }

    for (const auto &val: entityManager.getAllEntities() | std::views::values) {
        if (val->type == EntityType::Item) {
            auto item = std::static_pointer_cast<Item>(val);
            BoundingBox playerBox = player->getPickUpBox();
            BoundingBox itemBox = item->getHitBox();

            if (item->getCooldown() == 0 && playerBox.intersects(itemBox)) {
                const uint8_t itemsToAdd = player->canItemBeAddedToInventory(item->id(), item->getCount());
                if (itemsToAdd > 0) {
                    sendPickUpItem(item, player, itemsToAdd);
                    entityManager.removeEntity(item->uuidString);
                    player->addItemToInventory(item->id(), itemsToAdd);
                    break;
                }
            }
        }
    }
}

void handlePlayerCommand(SocketType socket, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    int32_t entityID = parseVarInt(packetData, index);
    int32_t actionID = parseVarInt(packetData, index);
    int32_t jumpBoost = parseVarInt(packetData, index);
    std::vector<MetadataEntry> entries;

    switch (actionID) {
        case 0: // Start Sneaking
        {
            // Update the player's state
            player->setSneaking(true);

            // Prepare the Metadata Entry for Flags (Index 0)
            entries.push_back({0, 0, std::vector<uint8_t>{ player->flags }}); // Flags

            // Prepare the Metadata Entry for Pose (Index 6)
            entries.push_back({6, 21}); // Sneaking
            writeVarInt(entries.at(1).value, 5); // Sneaking

            // Send the Entity Metadata Packet
            sendEntityMetadataPacket(player, entries, entityID);
            break;
        }
        case 1: // Stop Sneaking
        {
            // Update the player's flags
            player->setSneaking(false);

            // Prepare the Metadata Entry for Flags (Index 0)
            entries.push_back({0, 0, std::vector<uint8_t>{ player->flags }}); // Flags

            // Prepare the Metadata Entry for Pose (Index 6)
            entries.push_back({6, 21}); // Standing
            writeVarInt(entries.at(1).value, 0); // Standing


            // Send the Entity Metadata Packet
            sendEntityMetadataPacket(player, entries, entityID);
            break;
        }
        default: {
            logMessage("Received unknown player action ID: " + std::to_string(actionID), LOG_WARNING);
            break;
        }
    }
}

void handlePlayerSwingArm(SocketType socket, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    switch (const size_t hand = parseVarInt(packetData, index)) {
        case 0: // Main Hand
            sendEntityAnimation(player, SWING_MAIN_ARM);
            break;
        case 1: // Off Hand
            sendEntityAnimation(player, SWING_OFFHAND);
            break;
        default: // Unknown hand
            logMessage("Received unknown hand ID: " + std::to_string(hand), LOG_WARNING);
            break;
    }
}

void spawnBlockDrops(int32_t x, int32_t y, int32_t z, int16_t blockState) {
    std::vector<std::shared_ptr<Item>> items = getItemsFromBlock(blockState);
    for (const auto & item : items) {
        if (item->id() == 0) {
            return;
        }
        // Set initial position
        item->setPosition(x + 0.5, y + 0.5, z + 0.5);

        // Assign random initial motion
        double motionX = getRandomDouble(-0.1, 0.1); // Adjust range as needed
        double motionY = getRandomDouble(0.2, 0.3);    // Slight upward motion
        double motionZ = getRandomDouble(-0.1, 0.1);
        item->setMotion(motionX, motionY, motionZ);

        item->setCooldown(10); // 10 ticks before item can be picked up

        // Send spawn packet with velocity
        sendBundleDelimiter();
        sendSpawnEntityPacket(item);
        sendEntityMetadataPacket(item->getMetadata(), item->entityID);
        sendBundleDelimiter();
    }
}

void handleFinishedDigging(ClientConnection& client, const std::shared_ptr<Player> & player, const std::tuple<int32_t, int32_t, int32_t> & blockPosition, Block& block, const std::shared_ptr<Chunk>& chunk, Face face, size_t sequence, bool dropBlock) {
    int32_t x = std::get<0>(blockPosition);
    int32_t y = std::get<1>(blockPosition);
    int32_t z = std::get<2>(blockPosition);
    short oldBlockStateID = 0;

    // Lock the chunk for thread safety
    std::lock_guard lock(chunk->mutex);

    // If the block is already air, do nothing
    if (block.blockStateID == blocks["air"].defaultState) {
        return;
    }
    oldBlockStateID = block.blockStateID;

    // Remove the block (set to air)
    block.blockStateID = blocks["air"].defaultState;

    // Update block
    chunk->setBlock(getLocalCoordinate(x), y, getLocalCoordinate(z), block.blockStateID, true);

    // Notify all players viewing this chunk about the block change
    notifyChunkUpdate(chunk, x, y, z);

    // Acknowledge the block change
    sendAcknowledgeBlockChange(client, sequence);

    // Send world event packet to all viewers
    {
        ChunkCoordinates chunkCoords{chunk->chunkX, chunk->chunkZ};
        std::lock_guard lock1(chunkViewersMutex);
        auto it = chunkViewersMap.find(chunkCoords);
        if (it != chunkViewersMap.end()) {
            for (const auto & viewPlayer : it->second) {
                if(viewPlayer->uuid != player->uuid) {
                    sendWorldEventPacket(*viewPlayer->client, 2001, {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)}, oldBlockStateID);
                }
            }
        }
    }

    // Reset destroy stages for the block
    Position blockPos = Position{ x, y, z };
    sendBlockDestroyStage(player, blockPos, 10);

    // Send block drop items to the player (if not in creative mode)
    if (player->gameMode != CREATIVE && dropBlock) {
        spawnBlockDrops(x, y, z, oldBlockStateID);
    }
}

// TODO: Fire when player disconnects
void removePlayerFromAllChunks(const std::shared_ptr<Player> & player) {
    std::lock_guard lock(chunkViewersMutex);
    for (auto &viewers: chunkViewersMap | std::views::values) {
        std::erase(viewers, player);
    }
}

void miningScheduler(std::unordered_map<std::string, std::shared_ptr<Player>> &players, std::atomic<bool> &running) {
    const int tickRate = serverConfig.ticksPerSecond;
    while (running) {
        auto tickStart = std::chrono::steady_clock::now();

        for (auto &player: players | std::views::values) {
            for (auto it = player->currentMining.begin(); it != player->currentMining.end(); ) {
                MiningProgress &progress = it->second;
                double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - progress.startTime).count();

                // Calculate the current stage based on elapsed time
                int8_t newStage = static_cast<int8_t>((elapsed / progress.totalTime) * 10);
                newStage = std::min(newStage, static_cast<int8_t>(9)); // Stages 0-9

                if (newStage > progress.currentStage) {
                    // Send the new destroy stage
                    Position blockPos = progress.blockPos;
                    sendBlockDestroyStage(player, blockPos, newStage);
                    progress.currentStage = newStage;
                }

                if (elapsed >= progress.totalTime) {
                    // Mining complete
                    sendBlockDestroyStage(player, progress.blockPos, 10); // Remove destroy progress
                } else {
                    ++it;
                }
            }
        }

        // Sleep until the next tick
        auto tickEnd = std::chrono::steady_clock::now();
        std::chrono::duration<double> tickDuration = tickEnd - tickStart;
        double sleepTime = (1.0 / tickRate) - tickDuration.count();
        if (sleepTime > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(sleepTime));
        }
    }
}

void handlePlayerActions(ClientConnection& client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    auto action = static_cast<PlayerAction>(parseVarInt(packetData, index));
    uint64_t position = parseLong(packetData, index);
    int32_t x{};
    int32_t y{};
    int32_t z{};
    decodePosition(position, x, y, z);
    auto face = static_cast<Face>(parseByte(packetData, index));
    size_t sequence = parseVarInt(packetData, index);

    const auto chunk = getChunkContainingBlock(x, y, z);
    if (!chunk) {
        logMessage("Chunk not found for block position (" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")", LOG_ERROR);
        return;
    }

    Block block = chunk->getBlock(getLocalCoordinate(x), y, getLocalCoordinate(z));

    switch (action) {
        case STARTED_DIGGING: {
            DiggingInfo diggingInfo = calculateDiggingSpeed(block.blockStateID, player);
            if(player->gameMode == CREATIVE || (diggingInfo.diggingTime == 0 && diggingInfo.canHarvest)) {
                handleFinishedDigging(client, player, std::tuple(x, y, z), block, chunk, face, sequence, diggingInfo.canHarvest);
            } else if (diggingInfo.canHarvest || diggingInfo.diggingTime > 0) {
                // Initiate mining with destroy stages
                Position blockPos = Position{ x, y, z };
                MiningProgress progress;
                progress.startTime = std::chrono::steady_clock::now();
                progress.totalTime = diggingInfo.diggingTime; // Total time in seconds
                progress.currentStage = 0;
                progress.blockPos = blockPos;
                progress.sequence = sequence;
                progress.canHarvest = diggingInfo.canHarvest;

                {
                    std::lock_guard lock(player->miningMutex);
                    player->currentMining[blockPos] = progress;
                }

                // Send initial destroy stage
                sendBlockDestroyStage(player, blockPos, progress.currentStage);

                // Note: The miningScheduler thread will handle subsequent stages
            }
            break;
        }
        case CANCELLED_DIGGING: {
            Position blockPos = Position{ x, y, z };
            {
                std::lock_guard lock(player->miningMutex);
                auto it = player->currentMining.find(blockPos);
                if (it != player->currentMining.end()) {
                    // Reset the destroy stage
                    sendBlockDestroyStage(player, blockPos, 0);
                    player->currentMining.erase(it);
                }
            }
            break;
        }
        case FINISHED_DIGGING: {
            Position blockPos = Position{ x, y, z };
            bool shouldHandle = false;
            bool canHarvest = false;
            {
                std::lock_guard lock(player->miningMutex);
                auto it = player->currentMining.find(blockPos);
                if (it != player->currentMining.end()) {
                    shouldHandle = true;
                    canHarvest = it->second.canHarvest;
                    player->currentMining.erase(it);
                }
            }
            if (shouldHandle) {
                handleFinishedDigging(client, player, std::make_tuple(x, y, z), block, chunk, face, sequence, canHarvest);
            }
            break;
        }
        case DROP_ITEM_STACK:
            break;
        case DROP_ITEM:
            break;
        case SHOOT_ARROW_OR_FINISH_EATING:
            break;
        case SWAP_HELD_ITEMS:
            break;
        default:
            // Unknown action
            logMessage("Received unknown player action: " + std::to_string(static_cast<int>(action)), LOG_WARNING);
            break;
    }
}

void handleSetCreativeModeSlot(SocketType socket, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    short slot = parseShort(packetData, index);

    SlotData slotDataParsed;
    // 2.1 Item Count (VarInt)
    slotDataParsed.itemCount = parseVarInt(packetData, index);

    // 2.2 Item ID (Optional VarInt)
    if (slotDataParsed.itemCount > 0) {
        slotDataParsed.itemId = parseVarInt(packetData, index);
    } else {
        slotDataParsed.itemId = 0;
    }

    // 2.3 Number of Components to Add (Optional VarInt)
    if (slotDataParsed.itemCount > 0) {
        slotDataParsed.numCompToAdd = parseVarInt(packetData, index);
        logMessage("Number of components to add: " + std::to_string(*slotDataParsed.numCompToAdd), LOG_DEBUG);

        // 2.4 Components to Add (Array of Components)
        if (slotDataParsed.numCompToAdd && *slotDataParsed.numCompToAdd > 0) {
            std::vector<Component> componentsToAdd;
            for (size_t i = 0; i < *slotDataParsed.numCompToAdd; ++i) {
                Component comp = parseComponent(packetData, index);
                componentsToAdd.push_back(comp);
            }
            slotDataParsed.compsToAdd = componentsToAdd;
        }
    }

    // 2.5 Number of Components to Remove (Optional VarInt)
    if (slotDataParsed.itemCount > 0) {
        slotDataParsed.numCompToRemove = parseVarInt(packetData, index);

        // 2.6 Components to Remove (Array of Component Types)
        if (slotDataParsed.numCompToRemove && *slotDataParsed.numCompToRemove > 0) {
            std::vector<ComponentType> componentsToRemove;
            for (size_t i = 0; i < *slotDataParsed.numCompToRemove; ++i) {
                size_t compTypeVarInt = parseVarInt(packetData, index);
                componentsToRemove.emplace_back(static_cast<ComponentType>(compTypeVarInt));
            }
            slotDataParsed.compsToRemove = componentsToRemove;
        }
    }

    // Reference: https://wiki.vg/images/1/13/Inventory-slots.png
    if(player->activeSlot == (slot - 36)) {
        EquipmentSlot mainHandSlot;
        mainHandSlot.slotId = 0;
        mainHandSlot.slotData = slotDataParsed;
        player->equipment.mainHand = mainHandSlot;
        sendEquipmentPacket(player, player->entityID, mainHandSlot);
    }
    if(slot == 45) {
        EquipmentSlot offHandSlot;
        offHandSlot.slotId = 1;
        offHandSlot.slotData = slotDataParsed;
        player->equipment.offHand = offHandSlot;
        sendEquipmentPacket(player, player->entityID, offHandSlot);
    } else if(slot == 5) {
        EquipmentSlot helmetSlot;
        helmetSlot.slotId = 5;
        helmetSlot.slotData = slotDataParsed;
        player->equipment.helmet = helmetSlot;
        sendEquipmentPacket(player, player->entityID, helmetSlot);
    } else if(slot == 6) {
        EquipmentSlot chestplateSlot;
        chestplateSlot.slotId = 4;
        chestplateSlot.slotData = slotDataParsed;
        player->equipment.helmet = chestplateSlot;
        sendEquipmentPacket(player, player->entityID, chestplateSlot);
    } else if(slot == 7) {
        EquipmentSlot leggingsSlot;
        leggingsSlot.slotId = 3;
        leggingsSlot.slotData = slotDataParsed;
        player->equipment.helmet = leggingsSlot;
        sendEquipmentPacket(player, player->entityID, leggingsSlot);
    } else if(slot == 8) {
        EquipmentSlot bootsSlot;
        bootsSlot.slotId = 2;
        bootsSlot.slotData = slotDataParsed;
        player->equipment.helmet = bootsSlot;
        sendEquipmentPacket(player, player->entityID, bootsSlot);
    }

    player->inventory->slots[slot] = slotDataParsed;
    SendSetContainerSlot(*player->client, 0, 0, slot, slotDataParsed);
}

void handleSetHeldItem(SocketType socket, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    short slot = parseShort(packetData, index);
    player->activeSlot = slot;

    if(player->inventory->slots.contains(slot + 36)) {
        EquipmentSlot mainHandSlot;
        mainHandSlot.slotId = 0;
        mainHandSlot.slotData = player->inventory->slots.at(slot + 36);
        player->equipment.mainHand = mainHandSlot;
        sendEquipmentPacket(player, player->entityID, mainHandSlot);
    } else {
        player->inventory->slots[slot + 36] = SlotData();
        EquipmentSlot mainHandSlot;
        mainHandSlot.slotId = 0;
        mainHandSlot.slotData = player->inventory->slots.at(slot + 36);
        player->equipment.mainHand = mainHandSlot;
        sendEquipmentPacket(player, player->entityID, mainHandSlot);
    }
}

Position calculateTargetPosition(const Position& clickedBlock, Face face) {
    Position target = clickedBlock;
    switch (face) {
        case BOTTOM:
            target.y -= 1.0;
        break;
        case TOP:
            target.y += 1.0;
        break;
        case NORTH:
            target.z -= 1.0;
        break;
        case SOUTH:
            target.z += 1.0;
        break;
        case WEST:
            target.x -= 1.0;
        break;
        case EAST:
            target.x += 1.0;
        break;
    }
    return target;
}

bool isPositionValid(const Position& pos) {
    // TODO: Use actual world boundaries
    constexpr double minX = -30000000.0;
    constexpr double maxX = 30000000.0;
    constexpr double minY = 0.0;
    constexpr double maxY = 256.0;
    constexpr double minZ = -30000000.0;
    constexpr double maxZ = 30000000.0;

    return (pos.x >= minX && pos.x <= maxX) &&
           (pos.y >= minY && pos.y <= maxY) &&
           (pos.z >= minZ && pos.z <= maxZ);
}

bool isBlockReplaceable(Block block) {
    // TODO: Implement
    return false;
}

void openCraftingTable(const std::shared_ptr<Player> & player) {
    player->windowID++;
    player->windowID %= 100;
    if (player->windowID == 0) {
        player->windowID = 1;
    }
    auto craftingTableInv = std::make_shared<CraftingTableInventory>(player->windowID);
    craftingTableInv->copyPlayerInventory(*player->inventory);
    // Link this inventory to the player
    player->currentInventory = craftingTableInv;
    craftingTableInv->client = player->client;
    sendOpenScreen(*player->client, player->windowID, 12 /* Crafting Table */, "§0Crafting");
}

bool interact(const std::shared_ptr<Player> & player, const Position & blockPos, Face face, const Position & cursorPos, const SlotData & heldItem) {
    // Retrieve the chunk containing the block
    const auto chunk = getChunkContainingBlock(static_cast<int32_t>(blockPos.x), static_cast<int32_t>(blockPos.y), static_cast<int32_t>(blockPos.z));
    if (!chunk) {
        logMessage("Chunk not found for block position (" + std::to_string(blockPos.x) + ", " + std::to_string(blockPos.y) + ", " + std::to_string(blockPos.z) + ")", LOG_ERROR);
        return false;
    }
    // Get the block within the chunk
    uint16_t blockState = chunk->getBlock(getLocalCoordinate(static_cast<int32_t>(blockPos.x)), static_cast<int32_t>(blockPos.y), getLocalCoordinate(static_cast<int32_t>(blockPos.z))).blockStateID;

    if (blockState == blocks["crafting_table"].defaultState) {
        // Open crafting table
        openCraftingTable(player);
        return true;
    }

    return false;
}

void handleUseItemOn(ClientConnection& client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    // Hand (VarInt)
    size_t hand = parseVarInt(packetData, index);
    // Position (Long/Position)
    int64_t positionL = parseLong(packetData, index);
    Position blockPosition{};
    Position cursorPosition{};
    int32_t x{};
    int32_t y{};
    int32_t z{};
    decodePosition(positionL, x, y, z);
    blockPosition = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
    // Face (VarInt)
    Face face = static_cast<Face>(parseVarInt(packetData, index));
    // Cursor Position x (Float)
    float cursorX = parseFloat(packetData, index);
    // Cursor Position y (Float)
    float cursorY = parseFloat(packetData, index);
    // Cursor Position z (Float)
    float cursorZ = parseFloat(packetData, index);
    cursorPosition = {cursorX, cursorY, cursorZ};
    // Inside Block (Boolean)
    bool insideBlock = packetData[index++] != 0;
    // Sequence (VarInt)
    int32_t sequence = parseVarInt(packetData, index);

    // 2. Determine the Block to Place
    int selectedSlot = player->activeSlot + 36;
    SlotData heldItem = player->inventory->slots[selectedSlot];

    if (!player->isSneaking()) {
        if (interact(player, blockPosition, face, cursorPosition, heldItem)) {
            return;
        }
    }


    if (heldItem.itemId == items["air"].id) {
        // Player is holding 'air', nothing to place
        return;
    }

    // Check if the held item is a placeable block
    if (!blocks.contains(itemIDs[heldItem.itemId.value()].name)) {
        // Item is not placeable, possibly send a failure response
        return;
    }

    // 3. Calculate the Target Position for Block Placement
    Position targetPosition = calculateTargetPosition(blockPosition, face);

    // 4. Validate the Target Position
    if (!isPositionValid(targetPosition)) {
        // Invalid position, possibly send a failure response
        return;
    }

    // 5. Check if the Target Block is Replaceable
    // Retrieve the chunk containing the block
    const auto chunk = getChunkContainingBlock(static_cast<int32_t>(targetPosition.x), static_cast<int32_t>(targetPosition.y), static_cast<int32_t>(targetPosition.z));
    if (!chunk) {
        logMessage("Chunk not found for block position (" + std::to_string(targetPosition.x) + ", " + std::to_string(targetPosition.y) + ", " + std::to_string(targetPosition.z) + ")", LOG_ERROR);
        return;
    }

    // Lock the chunk for thread safety
    std::lock_guard lock(chunk->mutex);

    // Get the block within the chunk
    Block block = chunk->getBlock(getLocalCoordinate(static_cast<int32_t>(targetPosition.x)), static_cast<int32_t>(targetPosition.y), getLocalCoordinate(static_cast<int32_t>(targetPosition.z)));
    if (isBlockReplaceable(block)) {
        targetPosition = blockPosition;
        block = chunk->getBlock(getLocalCoordinate(static_cast<int32_t>(blockPosition.x)), static_cast<int32_t>(blockPosition.y), getLocalCoordinate(static_cast<int32_t>(blockPosition.z)));
    }

    // 6. Place the Block on the Server
    // 6.a. Assign Block States Based on Placement Context
    BlockData blockData = blocks[itemIDs[heldItem.itemId.value()].name];
    std::vector<BlockState> currentBlockState;
    assignCurrenBlockStates(blockData, player, targetPosition, face, cursorPosition, currentBlockState);
    block.blockStateID = static_cast<short>(calculateBlockStateID(blockData, currentBlockState));

    chunk->setBlock(getLocalCoordinate(static_cast<int32_t>(targetPosition.x)), static_cast<int32_t>(targetPosition.y), getLocalCoordinate(static_cast<int32_t>(targetPosition.z)), block.blockStateID, true);

    // 7. Notify Relevant Clients About the Block Change
    notifyChunkUpdate(chunk, static_cast<int32_t>(targetPosition.x), static_cast<int32_t>(targetPosition.y), static_cast<int32_t>(targetPosition.z));

    // Acknowledge the block change
    sendAcknowledgeBlockChange(client, sequence);

    // 8. Update the Player's Inventory
    // In Creative Mode, items are not consumed. If in Survival, decrease item count
    if (player->gameMode != 1) {
        if (player->inventory->getSlotData(selectedSlot).itemCount > 0) {
            player->inventory->getSlotData(selectedSlot).itemCount--;
            if (player->inventory->getSlotData(selectedSlot).itemCount == 0) {
                player->inventory->clearSlot(selectedSlot);
            }
        }
    }
}

EVP_PKEY* parsePublicKey(const std::vector<uint8_t>& pubKeyBytes) {
    const unsigned char* data = pubKeyBytes.data();
    EVP_PKEY* publicKey = d2i_PUBKEY(nullptr, &data, pubKeyBytes.size());
    if (!publicKey) {
        logMessage("Failed to parse public key: " + std::string(ERR_error_string(ERR_get_error(), nullptr)), LOG_ERROR);
        return nullptr;
    }
    return publicKey;
}

bool verifySessionKeySignature(const std::shared_ptr<Player> & player, long expires_at, const std::vector<uint8_t> & publicKey, const std::vector<uint8_t> & keySig, const std::vector<EVP_PKEY *> &mojangPublicKeys) {
    std::vector<uint8_t> dataBuffer;

    // Sender UUID as bytes
    dataBuffer.insert(dataBuffer.end(), player->uuid.begin(), player->uuid.end());

    // Expiration Timestamp (Long)
    writeLong(dataBuffer, expires_at);

    // Public Key (Array of Bytes)
    dataBuffer.insert(dataBuffer.end(), publicKey.begin(), publicKey.end());

    // Initialize OpenSSL verification context
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        logMessage("Failed to create EVP_MD_CTX.", LOG_ERROR);
        return false;
    }

    for (auto &pubKey : mojangPublicKeys) {
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        if (EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha1(), nullptr, pubKey) != 1) {
            EVP_MD_CTX_free(mdctx);
            continue; // Try the next key
        }

        if (EVP_DigestVerify(mdctx, keySig.data(), keySig.size(), dataBuffer.data(), dataBuffer.size()) == 1) {
            EVP_MD_CTX_free(mdctx);
            return true; // Verification succeeded
        }

        EVP_MD_CTX_free(mdctx);
    }
    return false; // Verification failed with all keys
}

void handlePlayerSession(ClientConnection & client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    // Read Session ID (UUID)
    player->sessionId = parseBytes(packetData, index, 16);

    // Read Public Key
    // Expire at (Long)
    player->sessionKey.expiresAt = parseLong(packetData, index);

    // Length of Public Key (VarInt)
    int32_t publicKeyLength = parseVarInt(packetData, index);

    // Public Key (Array of Bytes)
    player->sessionKey.pubKey = parseBytes(packetData, index, publicKeyLength);

    // Length of Key Signature (VarInt)
    int32_t keySignatureLength = parseVarInt(packetData, index);

    // Key Signature (Array of Bytes)
    player->sessionKey.keySig = parseBytes(packetData, index, keySignatureLength);

    if (serverConfig.enableSecureChat) {
        // Parse the public key and store it
        player->publicKey = parsePublicKey(player->sessionKey.pubKey);
        if (!player->publicKey) {
            disconnectClient(player, "Invalid public key.", true);
            return;
        }

        // Fetch Mojang's public keys
        std::vector<std::string> mojangPublicKeyPEMs = fetchMojangPublicKeys();
        if (mojangPublicKeyPEMs.empty()) {
            // Handle error
            disconnectClient(player, "Unable to fetch Mojang public keys.", true);
            return;
        }

        // Load and decode the public keys
        std::vector<EVP_PKEY *> mojangPublicKeys;
        for (const auto &keyBase64 : mojangPublicKeyPEMs) {
            if (EVP_PKEY *pubKey = parsePublicKey(base64Decode(keyBase64))) {
                mojangPublicKeys.push_back(pubKey);
            }
        }

        // Verify the player's session key
        if (mojangPublicKeys.empty()) {
            disconnectClient(player, "Failed to load Mojang public keys.", true);
            return;
        }

        // TODO: Fix key verification
        if (!verifySessionKeySignature(player, player->sessionKey.expiresAt, player->sessionKey.pubKey, player->sessionKey.keySig, mojangPublicKeys)) {
            logMessage("Player: " + player->name + " failed to verify session key signature.", LOG_ERROR);
            disconnectClient(player, "Invalid session key signature.", true);
            return;
        }

        std::vector<std::shared_ptr<Player>> playerInfo = {player};
        sendPlayerInfoUpdate(client, playerInfo, 0x02);
    }
}

bool verifyChatSignature(const std::shared_ptr<Player>& player, const std::string& message, long timestamp, long salt, int index, const std::vector<uint8_t>& signature, const std::vector<std::vector<uint8_t>>& previousSignatures) {
    if (!player->publicKey) {
        logMessage("Player does not have a valid public key.", LOG_ERROR);
        return false;
    }

    std::vector<uint8_t> dataBuffer;

    // 1. Sender UUID as string
    std::vector<uint8_t> uuidBytes;
    uuidBytes.insert(uuidBytes.end(), player->uuid.begin(), player->uuid.end());
    writeBytes(dataBuffer, uuidBytes);

    // 3. Session UUID as bytes
    writeBytes(dataBuffer, player->sessionId);

    // 4. Index (VarInt)
    writeVarInt(dataBuffer, index);

    // 5. Salt (Long)
    writeLong(dataBuffer, salt);

    // 6. Timestamp (Long)
    writeLong(dataBuffer, timestamp);

    // 8. Original chat content (String)
    writeString(dataBuffer, message);

    // 9. Length of previous messages (VarInt)
    writeVarInt(dataBuffer, static_cast<int32_t>(previousSignatures.size()));

    // 10. Previous message signatures (Byte Array)
    for (const auto& prevSig : previousSignatures) {
        writeBytes(dataBuffer, prevSig);
    }

    // Convert dataBuffer to a contiguous memory block
    std::string data(reinterpret_cast<char*>(dataBuffer.data()), dataBuffer.size());

    // Initialize OpenSSL verification context
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        logMessage("Failed to create EVP_MD_CTX.", LOG_ERROR);
        return false;
    }

    bool result = false;

    // Initialize verification with SHA-256
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, player->publicKey) != 1) {
        logMessage("EVP_DigestVerifyInit failed: " + std::string(ERR_error_string(ERR_get_error(), nullptr)), LOG_ERROR);
        EVP_MD_CTX_free(ctx);
        return false;
    }

    // Feed the data to the verifier
    if (EVP_DigestVerifyUpdate(ctx, data.c_str(), data.size()) != 1) {
        logMessage("EVP_DigestVerifyUpdate failed: " + std::string(ERR_error_string(ERR_get_error(), nullptr)), LOG_ERROR);
        EVP_MD_CTX_free(ctx);
        return false;
    }

    // Verify the signature
    int verifyResult = EVP_DigestVerifyFinal(ctx, signature.data(), signature.size());
    if (verifyResult == 1) {
        // Signature is valid
        result = true;
    } else if (verifyResult == 0) {
        // Signature is invalid
        logMessage("Signature verification failed: Invalid signature.", LOG_ERROR);
    } else {
        // Error occurred during verification
        logMessage("EVP_DigestVerifyFinal failed: " + std::string(ERR_error_string(ERR_get_error(), nullptr)), LOG_ERROR);
    }

    EVP_MD_CTX_free(ctx);
    return result;
}

void handleChatMessage(const ClientConnection & client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player, const RegistryManager& registryManager) {
    // Message (String)
    std::string message = parseString(packetData, index);

    // Timestamp (Long)
    int64_t timestamp = parseLong(packetData, index);

    // Salt (Long)
    int64_t salt = parseLong(packetData, index);

    // Has Signature (Boolean)
    bool hasSignature = packetData[index++] != 0;

    std::vector<uint8_t> signature;

    if (hasSignature) {
        signature = parseBytes(packetData, index, 256);
    }

    // Message Count (VarInt)
    int32_t messageCount = parseVarInt(packetData, index);

    // Acknowledged (Fixed Bit Set) // TODO: Implement

    if (hasSignature && serverConfig.enableSecureChat) {
        // Verify the signature
        std::vector<std::vector<uint8_t>> previousSignatures;
        if (!verifyChatSignature(player, message, timestamp, salt, 0, signature, previousSignatures)) {
            // Signature is invalid
            logMessage("Player " + player->name + " sent an invalid chat message signature", LOG_ERROR);
            return;
        }
    }

    // Broadcast the chat message to all players
    broadcastPlayerChatMessage(player, message, timestamp, salt, hasSignature ? &signature : nullptr, registryManager);
}

void handleCommand(ClientConnection & client, const std::shared_ptr<Player>& player, const std::string & command) {
    auto chatOutput = [player](const std::string& message, bool error, const std::vector<std::string>& args) {
        std::vector<std::shared_ptr<Player>> playerInfo = {player};
        sendTranslatedChatMessage(message, false, error ? "red" : "white", &playerInfo, false, &args);
    };
    CommandParser parser(globalCommandGraph, chatOutput);

    // Parse and execute the command
    bool success = parser.parseAndExecute(*player, command);
}

void handleConsoleCommand(const std::string & command) {
    auto consoleOutput = [](const std::string& message, bool error, const std::vector<std::string>& args) {
        std::string formattedMessage = removeFormattingCodes(message);
        std::vector<std::string> formattedArgs;
        for (const auto& arg : args) {
            formattedArgs.push_back(removeFormattingCodes(arg));
        }
        logMessage("[Console] " + getTranslationString(formattedMessage, consoleLang, formattedArgs), error? LOG_ERROR : LOG_RAW);
    };
    CommandParser parser(globalCommandGraph, consoleOutput);

    // Parse and execute the command
    bool success = parser.parseAndExecuteConsole(command, consoleOutput);
}

void handleChatCommand(ClientConnection & client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    std::string command = parseString(packetData, index);
    logMessage(player->name + " issued command: " + command, LOG_RAW);
    handleCommand(client, player, command);
}

void handlePluginMessage(const ClientConnection & client, const std::vector<uint8_t> & packetData, size_t index, Player& player) {
    std::string channel = parseString(packetData, index);

    if (channel == "minecraft:brand") {
        // Brand Plugin Message
        std::string brand = parseString(packetData, index);
        player.brand = brand;
    }
}

void handleResourcePackResponse(const ClientConnection & client, const std::vector<uint8_t> & packetData, size_t index, const std::shared_ptr<Player> & player) {
    // Resource Pack UUID (Bytes)
    std::vector<uint8_t> resourcePackUUID = parseBytes(packetData, index, 16);
    std::array<uint8_t, 16> resourcePackUUIDArray{};
    std::ranges::copy(resourcePackUUID, resourcePackUUIDArray.begin());
    std::string resourcePackUUIDStr = uuidToString(resourcePackUUIDArray);
    switch (size_t result = parseVarInt(packetData, index)) {
        case SUCCESSFULLY_DOWNLOADED:
            logMessage(player->name + " successfully downloaded resource pack with UUID: " + resourcePackUUIDStr, LOG_DEBUG);
            break;
        case DECLINED:
            logMessage(player->name + " declined resource pack with UUID: " + resourcePackUUIDStr, LOG_DEBUG);
            break;
        case FAILED_DOWNLOAD:
            logMessage(player->name + " failed to download resource pack with UUID: " + resourcePackUUIDStr, LOG_ERROR);
            break;
        case ACCEPTED:
            logMessage(player->name + " accepted resource pack with UUID: " + resourcePackUUIDStr, LOG_DEBUG);
            break;
        case DOWNLOADED:
            logMessage(player->name + " successfully loaded resource pack with UUID: " + resourcePackUUIDStr, LOG_DEBUG);
            break;
        case INVALID_URL:
            logMessage(player->name + " provided an invalid URL for resource pack with UUID: " + resourcePackUUIDStr, LOG_ERROR);
            break;
        case FAILED_TO_RELOAD:
            logMessage(player->name + " failed to reload resource pack with UUID: " + resourcePackUUIDStr, LOG_ERROR);
            break;
        case DISCARDED:
            logMessage(player->name + " discarded resource pack with UUID: " + resourcePackUUIDStr, LOG_DEBUG);
            break;
        default:
            logMessage(player->name + " sent an unknown resource pack response: " + std::to_string(result), LOG_WARNING);
            break;
    }
}

void handleCommandSuggestionsRequest(ClientConnection & client, const std::vector<uint8_t> & vector, size_t size, const std::shared_ptr<Player> & shared) {
    int32_t transactionID = parseVarInt(vector, size);
    std::string command = parseString(vector, size);
    if (command[0] == '/') {
        command = command.substr(1);
    }
    if (command.back() == ' ') {
        command.pop_back();
    }
    if (command.find("bossbar set") == 0 && command.find("color") != std::string::npos) {
        sendCommandSuggestionsResponse(client, transactionID, {"blue", "green", "pink", "purple", "red", "white", "yellow"}, command.length() + 2);
    } else if (command.find("bossbar set") == 0 && command.find("style") != std::string::npos) {
            sendCommandSuggestionsResponse(client, transactionID, {"notched_10", "notched_12", "notched_20", "notched_6", "progress"}, command.length() + 2);
    } else if (command.find("bossbar get") == 0 || command.find("bossbar set") == 0 || command.find("bossbar remove") == 0) {
        // Send BossBar suggestions
        std::vector<std::string> suggestions;
        for (const auto &bossBar: bossBars | std::views::values) {
            suggestions.push_back("minecraft:" + bossBar.getId());
        }
        sendCommandSuggestionsResponse(client, transactionID, suggestions, command.length() + 2);
    } else {
    }
}

void handleKeepAlive(const ClientConnection & client, const std::vector<uint8_t> & packet, size_t index, const std::shared_ptr<Player> & player) {
    int64_t keepAliveID = parseLong(packet, index);
    if (keepAliveID != client.keepAliveID) {
        logMessage("Keep Alive ID mismatch for player: " + player->name, LOG_WARNING);
    }
}

void handleClickContainer(const ClientConnection & client, const std::vector<uint8_t> & packet, size_t index, const std::shared_ptr<Player> & player) {
    uint8_t windowID = parseByte(packet, index);
    int32_t stateID = parseVarInt(packet, index);
    int16_t slot = parseShort(packet, index);
    int8_t button = parseByte(packet, index);
    int32_t mode = parseVarInt(packet, index);
    int32_t arrayLength = parseVarInt(packet, index);
    std::vector<std::pair<int16_t, SlotData>> changedSlotsFromClient;
    for (int i = 0; i < arrayLength; i++) {
        int16_t slotIndex = parseShort(packet, index);
        SlotData slotData = parseSlotData(packet, index);
        changedSlotsFromClient.emplace_back(slotIndex, slotData);
    }
    SlotData carriedItem = parseSlotData(packet, index);
    player->currentInventory->HandleInventoryClick(windowID, stateID, slot, button, mode, changedSlotsFromClient, carriedItem);
}

void handleCloseContainer(const ClientConnection & client, const std::vector<uint8_t> & packet, size_t index, const std::shared_ptr<Player> & player) {
    uint8_t windowID = parseByte(packet, index);
    if (windowID != 0 && windowID == player->windowID) {
        // check if shared ptr inherits from specific class
        if (std::shared_ptr<ExternalInventory> inventory = std::dynamic_pointer_cast<ExternalInventory>(
            player->currentInventory)) {
            inventory->copyToPlayerInventory(*player->inventory);
            logMessage("Player " + player->name + " closed container with window ID: " + std::to_string(windowID), LOG_DEBUG);
        }
        player->currentInventory = player->inventory;
    }
}

void handleClientPacket(ClientConnection& client, const std::vector<uint8_t>& packetData, const std::shared_ptr<Player>& player, const RegistryManager& registryManager) {
    size_t index = 0;

    // Handle packets based on their IDs
    switch (int32_t packetID = parseVarInt(packetData, index)) {
        case ZERO_PACKET: // Client Info / Teleport Confirm
            handleZeroPacket(client, packetData, index, player);
            break;
        case LOGIN_PLUGIN_RESPONSE: // Serverbound Plugin Message
        case PLUGIN_MESSAGE_PLAY:
            handlePluginMessage(client, packetData, index, *player);
            break;
        case CHAT_COMMAND: // Chat Command
            handleChatCommand(client, packetData, index, player);
            break;
        case CHAT_MESSAGE: // Chat Message
            handleChatMessage(client, packetData, index, player, registryManager);
            break;
        case PLAYER_SESSION: // Player Session
            handlePlayerSession(client, packetData, index, player);
            break;
        case COMMAND_SUGGESTIONS_REQUEST: // Command Suggestions Request
            handleCommandSuggestionsRequest(client, packetData, index, player);
            break;
        case CLICK_CONTAINER: // Click container slot
            handleClickContainer(client, packetData, index, player);
            break;
        case CLOSE_CONTAINER: // Close container
            handleCloseContainer(client, packetData, index, player);
            break;
        case SERVERBOUND_KEEP_ALIVE: // Keep Alive
            handleKeepAlive(client, packetData, index, player);
            break;
        case PLAYER_POSITION: // Player Position
            if (client.state != ClientState::AwaitingTeleportConfirm) {
                handlePlayerPosition(client, packetData, index, player);
            }
            break;
        case PLAYER_POSITION_AND_ROTATION: // Player Position and Rotation
            if (client.state != ClientState::AwaitingTeleportConfirm) {
                handlePlayerPositionAndRotationPacket(client, packetData, index, player);
            }
            break;
        case Player_ROTATION: // Player Rotation
            if (client.state != ClientState::AwaitingTeleportConfirm) {
                handlePlayerRotation(client.socket, packetData, index, player);
            }
            break;
        case PLAYER_ON_GROUND: // Player On Ground
            handlePlayerOnGround(client.socket, packetData, index, player);
            break;
        case PLAYER_ACTION: // Player actions
            handlePlayerActions(client, packetData, index, player);
            break;
        case PLAYER_COMMAND: // Player command
            handlePlayerCommand(client.socket, packetData, index, player);
            break;
        case RESOURCE_PACK_RESPONSE_PLAY: // Resource Pack Response
            handleResourcePackResponse(client, packetData, index, player);
            break;
        case HELD_ITEM: // Set Held Item
            handleSetHeldItem(client.socket, packetData, index, player);
            break;
        case CREATIVE_MODE_SLOT: // Set Creative Mode Slot
            handleSetCreativeModeSlot(client.socket, packetData, index, player);
            break;
        case SWING_ARM: // Swing arm
            handlePlayerSwingArm(client.socket, packetData, index, player);
            break;
        case USE_ITEM_ON: // Use Item On
            handleUseItemOn(client, packetData, index, player);
            break;
        default: // Unknown packet ID
            std::stringstream stringstream;
            stringstream << "Received unknown packet ID: 0x" << std::hex << packetID << std::dec; // Convert to hex and then back to dec
            std::string formattedMessage = stringstream.str();
            logMessage(formattedMessage, LOG_WARNING);
            break;
    }
}

void handlePlayState(ClientConnection& client, const std::shared_ptr<Player>& newPlayer, const RegistryManager& registryManager) {
    // Send Join Game packet
    sendJoinGamePacket(client, newPlayer->entityID);

    // Update recipes
    sendUpdateRecipes(client);

    // Add commands
    sendCommandsPacket(client);

    // Send Player Position and Look packet
    sendSynchronizePlayerPositionPacket(client, newPlayer);

    /// Send Player Info Update to the new player about all existing entities (excluding themselves)
    std::unordered_map<int32_t, std::shared_ptr<Entity>> existingEntities = entityManager.getAllEntities();
    std::vector<std::shared_ptr<Entity>> entitiesToInform;
    for (const auto &entity: existingEntities | std::views::values) {
        if (entity->uuidString != newPlayer->uuidString) {
            entitiesToInform.push_back(entity);
        }
    }

    if (!entitiesToInform.empty()) {
        std::vector<std::shared_ptr<Player>> playersToAdd;
        for (const auto& entity : entitiesToInform) {
            if (entity->type == EntityType::Player) {
                if (std::shared_ptr<Player> player = std::dynamic_pointer_cast<Player>(entity)) {
                    playersToAdd.push_back(player);
                }
            }
        }
        sendPlayerInfoUpdate(client, playersToAdd, 0x09); // 0x01: Add Player, 0x08: Update Listed
    }

    // Send Player Info Update to all existing clients about the new player
    std::vector<std::shared_ptr<Player>> newPlayerInfo = {newPlayer};
    {
        std::lock_guard lock(connectedClientsMutex);
        for (auto& [uuid, existingClient] : connectedClients) {
            if (uuid != newPlayer->uuidString) {
                sendPlayerInfoUpdate(*existingClient, newPlayerInfo, 0x09); // 0x01: Add Player, 0x08: Update Listed
            }
        }
    }

    // Send Player Info Update to the new player about themselves
    sendPlayerInfoUpdate(client, newPlayerInfo, 0x09); // 0x01: Add Player, 0x08: Update Listed

    // Send Spawn Entity packets
    // Send to the new client about existing players
    for (const auto &entity: existingEntities | std::views::values) {
        if (entity->uuidString != newPlayer->uuidString && entity->type == EntityType::Player) {
            sendSpawnEntityPacket(client, entity);
        }
    }

    // Send to existing clients about the new player
    {
        std::lock_guard lock(connectedClientsMutex);
        for (auto& [uuid, existingClient] : connectedClients) {
            if (uuid != newPlayer->uuidString) {
                sendSpawnEntityPacket(*existingClient, newPlayer);
            }
        }
    }
    {
        std::lock_guard lock(connectedClientsMutex);
        connectedClients[newPlayer->uuidString] = &client;
    }


    // Start Keep Alive loop in a separate thread
    std::thread keepAliveLoop(startKeepAliveLoop, std::ref(client));

    // Send Game Event: Start waiting for level chunks
    // According to the protocol, the value depends on the event
    // For Event 13 (StartWaitingForLevelChunks), the value is not explicitly defined
    // Set it to 0.0f as a default
    sendGameEventPacket(client, GameEvent::StartWaitingForLevelChunks, 0.0f);

    // Send Set Center Chunk packet with initial chunk coordinates
    sendSetCenterChunkPacket(client, newPlayer->currentChunkX, newPlayer->currentChunkZ);

    // Send Chunk Data Packets
    // Determine which chunks to send based on view distance
    int viewDistance = serverConfig.viewDistance; // TODO: Send to player
    int centerChunkX = newPlayer->currentChunkX;
    int centerChunkZ = newPlayer->currentChunkZ;


    // TODO: Use getChunksInView
    // Load and send chunks within view distance
    auto sendChunks = [&](int centerX, int centerZ, const std::shared_ptr<Player>& player) {
        // Measure time
        auto startTime = std::chrono::steady_clock::now();

        // Define the chunks to load and already loaded
        std::vector<ChunkCoordinates> chunksToLoad;
        std::vector<ChunkCoordinates> chunksAlreadyLoaded; {
            int chunkCount = 0;
            std::lock_guard lock(chunkMapMutex);
            for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
                for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                    // Skip the current chunk if needed
                    chunkCount++;
                    if (dx == 0 && dz == 0) continue;

                    int chunkX = centerX + dx;
                    int chunkZ = centerZ + dz;
                    ChunkCoordinates coords{chunkX, chunkZ};
                    auto it = globalChunkMap.find(coords);
                    if (it != globalChunkMap.end()) {
                        chunksAlreadyLoaded.emplace_back(coords);
                    } else {
                        chunksToLoad.emplace_back(coords);
                    }
                }
            }
        }

        // Vector to hold futures for loaded chunks
        std::vector<std::future<std::shared_ptr<Chunk>>> loadFutures;
        loadFutures.reserve(chunksToLoad.size());

        // Enqueue load/generate tasks
        for (const auto& coords : chunksToLoad) {
            loadFutures.emplace_back(
                threadPool.enqueue([coords]() -> std::shared_ptr<Chunk> {
                    // Attempt to load the chunk from disk
                    auto loadedChunk = loadChunkFromDisk(coords.chunkX, coords.chunkZ);
                    if (!loadedChunk) {
                        // Generate the chunk if loading failed
                        if (serverConfig.worldType == "flat") {
                            FlatWorldSettings settings = flatWorldPresets[serverConfig.flatWorldPreset];
                            int highestY;
                            loadedChunk = generateFlatChunk(settings, coords.chunkX, coords.chunkZ, highestY);
                        }
                    }
                    return loadedChunk;
                })
            );
        }

        // Vector to hold chunks to send (already loaded and newly loaded)
        std::vector<std::shared_ptr<Chunk>> chunksToSend;
        chunksToSend.reserve(chunksAlreadyLoaded.size() + chunksToLoad.size());

        // Collect already loaded chunks
        {
            std::lock_guard lock(chunkMapMutex);
            for (const auto& coords : chunksAlreadyLoaded) {
                auto it = globalChunkMap.find(coords);
                if (it != globalChunkMap.end()) {
                    chunksToSend.emplace_back(it->second);
                }
            }
        }

        // Collect loaded/generated chunks and update globalChunkMap
        for (size_t i = 0; i < loadFutures.size(); ++i) {
            if (std::shared_ptr<Chunk> chunk = loadFutures[i].get()) {
                {
                    std::lock_guard lock(chunkMapMutex);
                    globalChunkMap[chunksToLoad[i]] = chunk;
                }
                chunksToSend.emplace_back(chunk);
            }
        }

        // Vector to hold futures for sending chunks
        std::vector<std::future<void>> sendFutures;
        sendFutures.reserve(chunksToSend.size());

        // Enqueue send tasks
        for (const auto& chunk : chunksToSend) {
            sendFutures.emplace_back(
                threadPool.enqueue([chunk, &client]() -> void {
                    sendChunkDataToPlayer(client, chunk);
                })
            );
        }

        for (const auto& chunk : chunksToSend) {
            player->loadedChunks.insert({chunk->chunkX, chunk->chunkZ});
        }

        // Wait for all sends to complete
        for (auto& future : sendFutures) {
            future.get();
        }

        auto chunkCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        ).count();

        logMessage("Loaded and Sent " + std::to_string(chunksToSend.size()) + " chunks in " + std::to_string(chunkCreationTime) + " ms", LOG_DEBUG);

    };

    // Initialize the world border
    worldBorder.initialize(serverConfig.worldBorder);
    sendInitializeWorldBorder(client, worldBorder);

    // Send current chunk to the player
    sendCurrentChunkToPlayer(client, centerChunkX, centerChunkZ);

    updatePlayerChunkView(newPlayer, -1, -1, centerChunkX, centerChunkZ);

    // Asynchronously send chunks to avoid blocking
    std::thread(sendChunks, newPlayer->currentChunkX, newPlayer->currentChunkZ, newPlayer).detach();

    // Send Resource Packs
    sendResourcePacks(client);

    // Receive and handle client packets
    try {
        while (true) {
            std::vector<uint8_t> packetData;
            if (!readPacket(client, packetData)) {
                disconnectClient(newPlayer, "Player disconnected", false);
                break;
            }

            // Process incoming packets (e.g., Keep Alive Response)
            handleClientPacket(client, packetData, newPlayer, registryManager);

            std::this_thread::sleep_for(std::chrono::nanoseconds(100)); // Avoid high CPU usage
        }
    } catch (const std::exception& e) {
        disconnectClient(newPlayer, "Player disconnected", false);
        logMessage("Client disconnected with error: " + std::string(e.what()), LOG_ERROR);
    }

    // Clean up
    keepAliveLoop.join();
}

bool waitForAcknowledgeFinishConfiguration(const ClientConnection& client) {
    std::vector<uint8_t> packetData;
    if (!readPacket(client, packetData)) {
        return false;
    }

    size_t index = 0;
    int32_t packetID = parseVarInt(packetData, index);
    if (packetID != ACKNOWLEDGE_FINISH_CONFIGURATION) {
        // Expected Acknowledge Finish Configuration packet
        std::stringstream stringstream;
        stringstream << "Expected Acknowledge Finish Configuration packet. Instead received: 0x" << std::hex << packetID << std::dec;
        std::string formattedMessage = stringstream.str();
        logMessage(formattedMessage, LOG_ERROR);
        return false;
    }

    // No fields to parse

    return true;
}

bool waitForClientKnownPacks(const ClientConnection& client) {
    std::vector<uint8_t> packetData;
    if (!readPacket(client, packetData)) {
        return false;
    }

    size_t index = 0;
    int32_t packetID = parseVarInt(packetData, index);
    if (packetID != SERVERBOUND_KNOWN_PACKS) {
        // Expected Known Packs packet
        return false;
    }
    size_t packCount = parseVarInt(packetData, index);

    return true;
}

bool waitForClientPluginMessage(const ClientConnection & client, Player& player) {
    std::vector<uint8_t> packetData;
    if (!readPacketTimeout(client, packetData, 2, false)) {
        return false;
    }

    size_t index = 0;
    int32_t packetID = parseVarInt(packetData, index);
    if (packetID != LOGIN_PLUGIN_RESPONSE) {
        // Expected Plugin Message packet
        return false;
    }

    handlePluginMessage(client, packetData, index, player);

    return true;
}

bool waitforClientInfo(const ClientConnection & client, Player& player) {
    std::vector<uint8_t> packetData;
    if (!readPacket(client, packetData)) {
        return false;
    }

    size_t index = 0;
    int32_t packetID = parseVarInt(packetData, index);
    if (packetID != CLIENT_INFORMATION) {
        // Expected Client Info packet
        return false;
    }

    return handleClientInformation(player, packetData, index);
}

bool handleConfigurationState(ClientConnection& client, RegistryManager& registryManager, Player& player) {

    waitForClientPluginMessage(client, player);

    if (!waitforClientInfo(client, player)) {
        // Handle error or disconnection
        return false;
    }

    sendServerPluginMessages(client);

    sendFeatureFlags(client, {"minecraft:vanilla"});

    sendKnownPacksPacket(client);

    if (!waitForClientKnownPacks(client)) {
        // Handle error or disconnection
        return false;
    }

    // Send Registry Data packet
    sendRegistryDataPacket(client, registryManager);

    // Update tags
    sendUpdateTagsPacket(client);

    // Send server links
    sendServerLinksPacket(client);

    // Send Finish Configuration packet
    sendFinishConfigurationPacket(client);

    // Wait for Acknowledge Finish Configuration from the client
    if (!waitForAcknowledgeFinishConfiguration(client)) {
        logMessage("Failed to receive Acknowledge Finish Configuration packet", LOG_ERROR);
        return false;
    }

    return true;
}

void handleLoginRequest(ClientConnection& client, RegistryManager& registryManager) {
    // Read Login Start packet
    std::vector<uint8_t> packetData;
    if (!readUnencryptedPacket(client, packetData)) {
        return;
    }

    size_t index = 0;
    int32_t packetID = parseVarInt(packetData, index);
    if (packetID != 0x00) {
        return;
    }

    std::string playerName = parseString(packetData, index);
    // Set UUID for the new player
    std::vector<uint8_t> uuidBytesVec = parseBytes(packetData, index, 16);
    // Convert to std::array
    std::array<uint8_t, 16> uuidBytes = { };
    std::ranges::copy(uuidBytesVec, uuidBytes.begin());
    std::string playerUUID = bytesToUUIDString(uuidBytes);
    // Remove '-' characters from UUID string
    std::erase(playerUUID, '-');
    uuidBytes = stringUUIDToBytes(playerUUID);

    if (playerUUID.empty()) {
        playerUUID = fetchPlayerUUID(playerName);
        if (playerUUID.empty()) {
            logMessage("Failed to fetch player UUID for: " + playerName, LOG_ERROR);
            return;
        }
        uuidBytes = stringUUIDToBytes(playerUUID);
    }

     // ************ Encryption Start ************
    if (serverConfig.enableEncryption) {
        // Step 1: Generate a random verify token (16 bytes recommended)
        std::array<uint8_t, 16> verifyToken{};
        if (RAND_bytes(verifyToken.data(), verifyToken.size()) != 1) {
            logMessage("Failed to generate verify token", LOG_ERROR);
            return;
        }

        // Step 2: Get server's public key in DER format
        std::vector<uint8_t> serverPublicKeyDER = registryManager.getRSAKeyPair().getPublicKeyDER();

        // Step 3: Construct Encryption Request packet
        std::vector<uint8_t> encryptionRequestPacket;

        // Packet ID for Encryption Request
        writeVarInt(encryptionRequestPacket, 0x01);

        // Server ID (empty string for modern versions)
        writeString(encryptionRequestPacket, serverConfig.serverId);

        // Public Key Length and Public Key
        writeVarInt(encryptionRequestPacket, static_cast<int32_t>(serverPublicKeyDER.size()));
        encryptionRequestPacket.insert(encryptionRequestPacket.end(), serverPublicKeyDER.begin(), serverPublicKeyDER.end());

        // Verify Token Length and Verify Token
        writeVarInt(encryptionRequestPacket, verifyToken.size());
        encryptionRequestPacket.insert(encryptionRequestPacket.end(), verifyToken.begin(), verifyToken.end());

        // Should authenticate (boolean)
        writeByte(encryptionRequestPacket, serverConfig.onlineMode);

        // Send Encryption Request packet
        if (!sendUnencryptedPacket(client, encryptionRequestPacket)) {
            logMessage("Failed to send Encryption Request packet", LOG_ERROR);
            return;
        }

        // Step 4: Receive Encryption Response packet
        std::vector<uint8_t> encryptionResponseData;
        if (!readUnencryptedPacket(client, encryptionResponseData)) {
            logMessage("Failed to read Encryption Response packet", LOG_ERROR);
            return;
        }

        size_t respIndex = 0;
        int32_t respPacketID = parseVarInt(encryptionResponseData, respIndex);
        if (respPacketID != 0x01) {
            logMessage("Invalid Encryption Response packet ID: " + std::to_string(respPacketID) + " (expected 0x01)", LOG_ERROR);
            return;
        }

        // Parse Encryption Response
        // Encrypted Shared Secret
        int32_t encryptedSharedSecretLength = parseVarInt(encryptionResponseData, respIndex);
        std::vector<uint8_t> encryptedSharedSecret = parseBytes(encryptionResponseData, respIndex, encryptedSharedSecretLength);

        // Encrypted Verify Token
        int32_t encryptedVerifyTokenLength = parseVarInt(encryptionResponseData, respIndex);
        std::vector<uint8_t> encryptedVerifyToken = parseBytes(encryptionResponseData, respIndex, encryptedVerifyTokenLength);

        // Step 5: Decrypt Shared Secret and Verify Token using server's private key
        std::vector<uint8_t> decryptedSharedSecret = registryManager.getRSAKeyPair().decrypt(encryptedSharedSecret);
        std::vector<uint8_t> decryptedVerifyToken = registryManager.getRSAKeyPair().decrypt(encryptedVerifyToken);

        // Step 6: Verify that the decrypted verify token matches the original
        if (decryptedVerifyToken.size() != verifyToken.size() ||
            std::memcmp(decryptedVerifyToken.data(), verifyToken.data(), verifyToken.size()) != 0) {
            logMessage("Verify token mismatch", LOG_ERROR);
            return;
            }

        // Step 7: Store the shared secret
        if (decryptedSharedSecret.size() < 16) { // AES-128 requires at least 16 bytes
            logMessage("Invalid shared secret size", LOG_ERROR);
            return;
        }
        std::copy_n(decryptedSharedSecret.begin(), 16, client.sharedSecret.begin());

        // Step 8: Initialize AES/CFB8 encryption and decryption contexts
        client.encryptCtx = EVP_CIPHER_CTX_new();
        client.decryptCtx = EVP_CIPHER_CTX_new();
        if (!client.encryptCtx || !client.decryptCtx) {
            logMessage("Failed to create AES cipher contexts", LOG_ERROR);
            sendDisconnectionPacket(client, "Failed to create AES cipher contexts");
            return;
        }

        // Initialize encryption context (AES-128-CFB8)
        if (EVP_EncryptInit_ex(client.encryptCtx, EVP_aes_128_cfb8(), nullptr, client.sharedSecret.data(), client.sharedSecret.data()) != 1) {
            logMessage("Failed to initialize AES encryption context", LOG_ERROR);
            sendDisconnectionPacket(client, "Failed to initialize AES encryption context");
            return;
        }

        // Initialize decryption context (AES-128-CFB8)
        if (EVP_DecryptInit_ex(client.decryptCtx, EVP_aes_128_cfb8(), nullptr, client.sharedSecret.data(), client.sharedSecret.data()) != 1) {
            logMessage("Failed to initialize AES decryption context", LOG_ERROR);
            sendDisconnectionPacket(client, "Failed to initialize AES decryption context");
            return;
        }
    }

    // ************ Encryption Setup Complete ************

    // ************ Step 3: Online Mode Authentication ************
    std::pair<std::string, std::string> texturesPair;
    if (serverConfig.onlineMode) {
        // Step 3.1: Compute Server Hash
        std::string serverId = serverConfig.serverId; // Ensure you have initialized server_id
        std::string serverHash = computeServerHash(serverId, client.sharedSecret, registryManager.getRSAKeyPair().getPublicKeyDER());

        // Step 3.2: Get Client's IP Address
        std::string clientIP = getClientIPAddress(client);

        // Step 3.3: Authenticate with Mojang
        std::string authenticatedUUID;
        std::string authenticatedName;
        bool authSuccess = authenticatePlayer(playerName, serverHash, clientIP, authenticatedUUID, authenticatedName, texturesPair);

        if (!authSuccess) {
            sendDisconnectionPacket(client, "Authentication with Mojang failed. Disconnecting.");
            return;
        }

        // Verify that the authenticatedName matches the provided playerName
        if (authenticatedName != playerName) {
            logMessage("Player name mismatch: " + authenticatedName + " != " + playerName, LOG_ERROR);
            sendDisconnectionPacket(client, "Player name mismatch. Disconnecting.");
            return;
        }

        playerUUID = authenticatedUUID;
        logMessage("Player authenticated: " + authenticatedName + " (" + authenticatedUUID + ")", LOG_DEBUG);
    } else {
        // Offline Mode: Use the client-provided UUID
        logMessage("Offline Mode: Using client-provided UUID: " + playerUUID, LOG_DEBUG);
    }


    if (serverConfig.enableCompression) {
        sendSetCompressionPacket(client, serverConfig.compressionThreshold);
        client.compressionEnabled = true;
    }

    // Create a new Player instance
    std::shared_ptr<Player> newPlayer = EntityFactory::createPlayer(uuidBytes, playerName);

    // Assign a unique Entity ID
    newPlayer->uuidString = playerUUID;
    newPlayer->properties = {}; // Populate as needed
    newPlayer->gameMode = CREATIVE;
    newPlayer->listed = true;
    newPlayer->ping = -1; // Unknown initially
    newPlayer->setClient(&client);
    if (texturesPair.first.empty() || texturesPair.second.empty()) {
        texturesPair = fetchPlayerSkin(newPlayer->uuidString);
        if (!texturesPair.first.empty() && !texturesPair.second.empty()) {
            newPlayer->properties["textures"] = { texturesPair.first, texturesPair.second };
        } else {
            // Assign a default skin if fetching fails
            std::string defaultSkin = "eyJ0aW1lc3RhbXAiOjE1OTA0ODEyMDAwMDAsInByb2ZpbGVJZCI6InV1aWQiLCJwcm9maWxlTmFtZSI6InBsYXllck5hbWUiLCJ0ZXh0dXJlcyI6eyJTS0lOIjp7InVybCI6Imh0dHA6Ly90ZXh0dXJlcy5taW5lY3JhZnQubmV0L3RleHR1cmUvczg2YzIyYjYyMmY2Y2E3ZTJmZjhkNTYyN2FkNDMxMDgyMjYyYzE5ZTM5MjMxYjI1YjczNGNiZDI0N2EwMjA4ZCJ9fX0=";
            newPlayer->properties["textures"] = { defaultSkin, "" };
        }
    } else {
        newPlayer->properties["textures"] = { texturesPair.first, texturesPair.second };
    }

    // Add the new player to the global player list
    {
        std::lock_guard lock(playersMutex);
        globalPlayers[playerUUID] = newPlayer;
        globalPlayersName[playerName] = newPlayer;
    }

    // Increase player count
    ++playerCount;

    // Send Login Success packet
    std::vector<uint8_t> responseData;
    responseData.push_back(LOGIN_SUCCESS);

    // Append UUID (16 bytes)
    responseData.insert(responseData.end(), uuidBytes.begin(), uuidBytes.end());

    // Append Username (String)
    writeString(responseData, newPlayer->name);

    // Number Of Properties (VarInt)
    writeVarInt(responseData, 0); // For now no properties

    // Strict Error Handling (Boolean)
    responseData.push_back(0x00); // TODO: Only in versions before 1.21.2 and 1.20.5+

    // Build and send the packet
    sendPacket(client, responseData);

    // ************ Wait for Login Acknowledged Packet ************
    std::vector<uint8_t> ackPacket;
    if (!readPacket(client, ackPacket)) {
        logMessage("Failed to read Login Acknowledged packet", LOG_ERROR);
        return;
    }

    size_t ackIndex = 0;
    int32_t ackPacketID = parseVarInt(ackPacket, ackIndex);
    if (ackPacketID != LOGIN_ACKNOWLEDGE) {
        logMessage("Expected Login Acknowledged packet (ID 0x03), but received packet ID: " + std::to_string(ackPacketID), LOG_ERROR);
        return;
    }

    // Now in Configuration state
    client.state = ClientState::Configuration;
    if(!handleConfigurationState(client, registryManager, *newPlayer)) {
        return;
    }

    sendTranslatedChatMessage("multiplayer.player.joined", false, "yellow", nullptr, true, newPlayer->name);

    // Now in Play state
    client.state = ClientState::Play;
    handlePlayState(client, newPlayer, registryManager);
}

void handleClient(SocketType clientSocket) {
    ClientConnection client;
    client.socket = clientSocket;
    client.state = ClientState::Handshake;

    // Set a timeout for the socket
#ifdef _WIN32
    DWORD timeout = 5000; // 5 seconds
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    try {
        // Read initial packet (Handshake)
        std::vector<uint8_t> packetData;
        if (!readUnencryptedPacket(client, packetData)) {
            // Close socket and return
            return;
        }

        size_t index = 0;
        int32_t packetID = parseVarInt(packetData, index);

        if (packetID == HANDSHAKE) {
            // Handshake packet
            int32_t protocolVersion = parseVarInt(packetData, index);
            std::string serverAddress = parseString(packetData, index);
            uint16_t serverPort = (packetData[index] << 8) | packetData[index + 1];
            index += 2;
            int32_t nextState = parseVarInt(packetData, index);

            if (nextState == 1) {
                // Status Request
                handleStatusRequest(client);
            } else if (nextState == 2) {
                RegistryManager registryManager;
                // Login Request
                handleLoginRequest(client, registryManager);
            }
        } else {
            logMessage("Invalid Handshake packet ID: " + std::to_string(packetID), LOG_ERROR);
            return;
        }
    } catch (const std::exception& e) {
        logMessage("Client disconnected with error: " + std::string(e.what()), LOG_ERROR);
        sendDisconnectionPacket(client, "Disconnected with error");
    }

    // Close socket
#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}



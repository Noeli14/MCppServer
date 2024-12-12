#include "clientbound_packets.h"

#include "registries/biome.h"
#include "core/config.h"
#include "registries/damage_type.h"
#include "registries/dimension_type.h"
#include "network.h"
#include "packet_ids.h"
#include "registries/painting_variant.h"
#include "entities/player.h"
#include "registries/registry_manager.h"
#include "core/server.h"
#include "core/utils.h"
#include "entities/item_entity.h"
#include "registries/wolf_variant.h"
#include "utils/translation.h"
#include "world/boss_bar.h"

void sendRemoveEntityPacket(const int32_t& entityID) {
    std::vector<uint8_t> packetData;
    packetData.push_back(REMOVE_ENTITIES);

    // Number of Entities (VarInt)
    writeVarInt(packetData, 1);

    // Entity IDs (VarInt)
    writeVarInt(packetData, entityID);

    // Build and send the packet to all clients
    broadcastToOthers(packetData);
}

void sendPlayerInfoRemove(const std::shared_ptr<Player>& player) {
    std::vector<uint8_t> packetData;
    packetData.push_back(PLAYER_INFO_REMOVE);

    // Number Of Players (VarInt)
    writeVarInt(packetData, 1); // Removing one player

    // Player UUID (16 bytes)
    packetData.insert(packetData.end(), player->uuid.begin(), player->uuid.end());

    // Send to all connected clients
    std::lock_guard lock(connectedClientsMutex);
    for (auto &existingClient: connectedClients | std::views::values) {
        sendPacket(*existingClient, packetData);
    }
}

void sendRegistryDataPacket(ClientConnection& client, RegistryManager& registryManager) {
    std::vector<uint8_t> packetData;
    packetData.push_back(REGISTRY_DATA);

    // --- Registry 1: dimension_type ---
    writeString(packetData, "minecraft:dimension_type"); // Registry Identifier

    // 5. Data (NBT)
    std::vector<DimensionType> dimensions;
    if (!loadDimensionTypesFromCompoundFile("../resources/registry_data.json", dimensions)) {
        logMessage("Failed to load dimension data.", LOG_ERROR);
        return;
    }

    // Number of entries in dimension_type
    writeVarInt(packetData, static_cast<int32_t>(dimensions.size()));

    // Iterate over each dimension type and serialize
    for (const auto& dimension : dimensions) {
        // 1. Entry Identifier (String)
        writeString(packetData, dimension.identifier);

        // 2. Has Data (Boolean)
        packetData.push_back(0x01); // true

        // 3. Data (NBT)
        nbt::tag_compound nbtDimenionType = dimension.serialize();
        std::vector<uint8_t> nbtDimenionTypeData = serializeNBT(nbtDimenionType, true);

        // Append the NBT data
        packetData.insert(packetData.end(), nbtDimenionTypeData.begin(), nbtDimenionTypeData.end());
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // --- Registry 2: biome ---
    packetData.clear();
    packetData.push_back(REGISTRY_DATA);
    writeString(packetData, "minecraft:worldgen/biome"); // Registry Identifier

    // Load biomes
    std::vector<BiomeRegistryEntry> biomeEntries;
    if (!loadBiomesFromCompoundFile("../resources/registry_data.json", biomeEntries)) {
        logMessage("Failed to load biomes from registry_data.json.", LOG_ERROR);
        return;
    }

    // Number of entries in biome
    int numBiomes = 0;
    for (const auto& entry : biomeEntries) {
        if (entry.type == BiomeRegistryEntry::Type::Biome && entry.biome.has_value()) {
            numBiomes++;
        }
    }
    writeVarInt(packetData, numBiomes);

    // Iterate over each biome and serialize
    for (const auto& entry : biomeEntries) {
        if (entry.type == BiomeRegistryEntry::Type::Biome && entry.biome.has_value()) {
            const Biome& biome = entry.biome.value();

            // 1. Entry Identifier (String) - e.g., "badlands"
            writeString(packetData, biome.identifier);

            // 2. Has Data (Boolean)
            packetData.push_back(0x01); // true

            // 3. Data (NBT)
            nbt::tag_compound nbtBiome = biome.serialize();
            std::vector<uint8_t> nbtBiomeData = serializeNBT(nbtBiome, true);
            // Append the NBT data
            packetData.insert(packetData.end(), nbtBiomeData.begin(), nbtBiomeData.end());
        } else if (entry.type == BiomeRegistryEntry::Type::Tag && entry.tag.has_value()) {
            continue;
        } else {
            logMessage("Unknown BiomeRegistryEntry type.", LOG_ERROR);
            continue;
        }
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // --- Registry 3: painting_variant ---
    packetData.clear();
    packetData.push_back(REGISTRY_DATA);
    writeString(packetData, "minecraft:painting_variant"); // Registry Identifier

    // Load painting variants
    std::vector<PaintingVariant> paintingVariants;
    if (!loadPaintingVariantsFromCompoundFile("../resources/registry_data.json", paintingVariants)) {
        logMessage("Failed to load painting variants from registry_data.json.", LOG_ERROR);
        return;
    }

    // Number of entries in painting_variant
    writeVarInt(packetData, static_cast<int32_t>(paintingVariants.size()));

    // Iterate over each painting variant and serialize
    for (const auto& variant : paintingVariants) {
        // 1. Entry Identifier (String) - e.g., "kebab"
        writeString(packetData, variant.asset_id);

        // 2. Has Data (Boolean)
        packetData.push_back(0x01); // true

        // 3. Data (NBT)
        nbt::tag_compound nbtPaintingVariant = variant.serialize();
        std::vector<uint8_t> nbtPaintingVariantData = serializeNBT(nbtPaintingVariant, true);

        // Append the NBT data
        packetData.insert(packetData.end(), nbtPaintingVariantData.begin(), nbtPaintingVariantData.end());
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // --- Registry 4: wolf_variant ---
    packetData.clear();
    packetData.push_back(REGISTRY_DATA);
    writeString(packetData, "minecraft:wolf_variant"); // Registry Identifier

    // Load wolf variants
    std::vector<WolfVariant> wolfVariants;
    if (!loadWolfVariantsFromCompoundFile("../resources/registry_data.json", wolfVariants)) {
        logMessage("Failed to load wolf variants from registry_data.json.", LOG_ERROR);
        return;
    }

    // Number of entries in wolf_variant
    writeVarInt(packetData, static_cast<int32_t>(wolfVariants.size()));

    // Iterate over each wolf variant and serialize
    for (const auto& variant : wolfVariants) {
        // 1. Entry Identifier (String)
        writeString(packetData, variant.identifier);

        // 2. Has Data (Boolean)
        packetData.push_back(0x01); // true

        // 3. Data (NBT)
        nbt::tag_compound nbtWolfVariant = variant.serialize();
        std::vector<uint8_t> nbtWolfVariantData = serializeNBT(nbtWolfVariant, true);

        // Append the NBT data
        packetData.insert(packetData.end(), nbtWolfVariantData.begin(), nbtWolfVariantData.end());
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // --- Registry 5: damage_type ---
    packetData.clear();
    packetData.push_back(REGISTRY_DATA);
    writeString(packetData, "minecraft:damage_type"); // Registry Identifier

    // Load damage types
    std::vector<DamageType> damageTypes;
    if (!loadDamageTypesFromCompoundFile("../resources/registry_data.json", damageTypes)) {
        logMessage("Failed to load damage types from registry_data.json.", LOG_ERROR);
        return;
    }

    // Number of entries in damage_type
    writeVarInt(packetData, static_cast<int32_t>(damageTypes.size()));

    // Iterate over each damage type and serialize
    for (const auto& type : damageTypes) {
        // 1. Entry Identifier (String)
        writeString(packetData, type.identifier);

        // 2. Has Data (Boolean)
        packetData.push_back(0x01); // true

        // 3. Data (NBT)
        nbt::tag_compound nbtDamageType = type.serialize();
        std::vector<uint8_t> nbtDamageTypeData = serializeNBT(nbtDamageType, true);

        // Append the NBT data
        packetData.insert(packetData.end(), nbtDamageTypeData.begin(), nbtDamageTypeData.end());
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // --- Registry 6: chat_type ---
    packetData.clear();
    packetData.push_back(REGISTRY_DATA);
    writeString(packetData, "minecraft:chat_type"); // Registry Identifier

    std::vector<ChatType> chatTypes = {
        {
            "minecraft:chat",
            "chat.type.text",
            {"sender", "content"} // senderName, message
        },
        {
            "minecraft:system",
            "chat.type.system",
            {"content"} // message
        },
        {
            "minecraft:announcement",
            "chat.type.announcement",
            {"sender"} // announcement message
        }
    };

    // Number of entries in chat_types
    writeVarInt(packetData, static_cast<int32_t>(chatTypes.size()));

    // 3. Entries (Array of Entry IDs)
    for (const auto& chatType : chatTypes) {
        // a. Entry Identifier (String)
        writeString(packetData, chatType.identifier);

        // b. Has Data (Boolean)
        packetData.push_back(0x01); // true

        // c. Data (NBT)
        nbt::tag_compound nbtChatType = chatType.serialize();
        std::vector<uint8_t> nbtChatTypeData = serializeNBT(nbtChatType, true);

        // Append the NBT data
        packetData.insert(packetData.end(), nbtChatTypeData.begin(), nbtChatTypeData.end());

        registryManager.addRegistryEntry("minecraft:chat_type", chatType.identifier);
    }

    // Build and send the packet
    sendPacket(client, packetData);
}

void sendWorldEventPacket(ClientConnection& client, const int& worldEvent, const Position& position, const int& data) {
    std::vector<uint8_t> packetData;
    packetData.push_back(WORLD_EVENT);

    // World Event (Int)
    writeInt(packetData, worldEvent);

    // Position (Long)
    writeLong(packetData, static_cast<int64_t>(encodePosition(static_cast<int32_t>(position.x), static_cast<int32_t>(position.y), static_cast<int32_t>(position.z))));

    // Event Data (Int)
    writeInt(packetData, data);

    // Disable Relative Volume
    if(worldEvent == 1023 || worldEvent == 1028 || worldEvent == 1038) {
        packetData.push_back(0x01);
    } else {
        packetData.push_back(0x00);
    }

    // Build and send the packet
    sendPacket(client, packetData);
}

bool sendUpdateTagsPacket(ClientConnection& client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(UPDATE_TAGS);

    // Number of Tags to update
    writeVarInt(packetData, 2);

    // --- Registry 1: minecraft:worldgen/biome ---
    writeString(packetData, "minecraft:worldgen/biome"); // Registry Identifier

    std::vector<BiomeRegistryEntry> biomeEntries;
    if (!loadBiomesFromCompoundFile("../resources/registry_data.json", biomeEntries)) {
        logMessage("Failed to load biomes from registry_data.json.", LOG_ERROR);
        return false;
    }

    // Number of entries in biome
    int numTags = 0;
    for (const auto& entry : biomeEntries) {
        if (entry.type == BiomeRegistryEntry::Type::Tag && entry.tag.has_value()) {
            numTags++;
        }
    }
    writeVarInt(packetData, numTags);

    // Iterate over each biome and serialize
    for (const auto& entry : biomeEntries) {
        if (entry.type == BiomeRegistryEntry::Type::Tag && entry.tag.has_value()) {
            const BiomeTag& tag = entry.tag.value();

            // 1. Tag Name (String)
            writeString(packetData, tag.identifier);

            // 2. Length of the entries list (VarInt)
            writeVarInt(packetData, static_cast<int32_t>(tag.biomes.size()));

            // 3. Entries (Array of VarInt)
            for (const auto& biome : tag.biomes) {
                auto id = biomes[stripNamespace(biome)].id;
                writeVarInt(packetData, id);
            }
        }
    }

    // --- Registry 2: minecraft:block ---
    writeString(packetData, "minecraft:block"); // Registry Identifier

    writeVarInt(packetData, blockTags.size());
    for (const auto& [tag, blockIDs] : blockTags) {
        writeString(packetData, tag);
        writeVarInt(packetData, static_cast<int32_t>(blockIDs.size()));
        for (const auto& blockID : blockIDs) {
            writeVarInt(packetData, blockID);
        }
    }

    // Build and send the packet
    sendPacket(client, packetData);

    return true;
}

void sendJoinGamePacket(ClientConnection& client, int32_t entityID) {
    std::vector<uint8_t> packetData;
    packetData.push_back(LOGIN);

    // 1. Entity ID (Int)
    writeInt(packetData, entityID);

    // 2. Is Hardcore (Boolean)
    packetData.push_back(0x00); // false

    // 3. Dimension Count (VarInt)
    writeVarInt(packetData, 1);

    // 4. Dimension Names (Array of Identifiers)
    writeString(packetData, "minecraft:overworld");

    // 5. Max Players (VarInt) - Deprecated, but still required
    writeVarInt(packetData, 100);

    // 6. View Distance (VarInt)
    writeVarInt(packetData, serverConfig.viewDistance);

    // 7. Simulation Distance (VarInt)
    writeVarInt(packetData, 10);

    // 8. Reduced Debug Info (Boolean)
    packetData.push_back(0x00); // false

    // 9. Enable Respawn Screen (Boolean)
    packetData.push_back(0x01); // true

    // 10. Do Limited Crafting (Boolean)
    packetData.push_back(0x00); // false

    // 11. Dimension Type (VarInt)
    writeVarInt(packetData, 0);

    // 12. Dimension Name (Identifier)
    writeString(packetData, "minecraft:overworld");

    // 13. Hashed Seed (Long)
    writeLong(packetData, 0);

    // 14. Game Mode (Unsigned Byte)
    packetData.push_back(0x01); // Survival mode (0: Survival, 1: Creative, 2: Adventure, 3: Spectator)

    // 15. Previous Game Mode (Byte)
    packetData.push_back(0xFF); // -1 (undefined)

    // 16. Is Debug (Boolean)
    packetData.push_back(0x00); // false

    // 17. Is Flat (Boolean)
    packetData.push_back(0x00); // false

    // 18. Has Death Location (Boolean)
    packetData.push_back(0x00); // false (so next fields are omitted)

    // 19. Portal Cooldown (VarInt)
    writeVarInt(packetData, 0);

    // 20. Enforces Secure Chat (Boolean)
    packetData.push_back(serverConfig.enableSecureChat);

    //packetData.push_back(0x00); // 0 (unused, required in 1.21.3)

    // Build and send the packet
    sendPacket(client, packetData);
}

void sendSynchronizePlayerPositionPacket(ClientConnection& client, const std::shared_ptr<Player> &player) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SYNCHRONIZE_PLAYER_POSITION);

    if(player->newSpawn) {
        // If the player's position is not set, use the spawn position
        player->position = spawnPosition;
    }

    // Set current chunk
    player->currentChunkX = getChunkCoordinate(player->position.x);
    player->currentChunkZ = getChunkCoordinate(player->position.z);

    // X, Y, Z (Double)
    writeDouble(packetData, player->position.x); // X
    writeDouble(packetData, player->position.y); // Y
    writeDouble(packetData, player->position.z); // Z

    float yaw = 0.0f;
    float pitch = 0.0f;

    // Yaw, Pitch (Float)
    writeFloat(packetData, 0.0f); // Yaw
    writeFloat(packetData, 0.0f); // Pitch

    player->rotation.yaw = yaw;
    player->rotation.pitch = pitch;
    player->rotation.headYaw = yaw;

    // Flags (Byte)
    packetData.push_back(0x00);

    // Teleport ID (VarInt)
    int32_t teleportID = generateUniqueTeleportID();
    writeVarInt(packetData, teleportID);

    // Add teleport ID to pending confirmations
    {
        std::lock_guard lock(client.mutex);
        client.pendingTeleportIDs.insert(teleportID);
    }

    // Build and send the packet
    sendPacket(client, packetData);

    // Update client state to AwaitingTeleportConfirm
    client.state = ClientState::AwaitingTeleportConfirm;
}

void sendEntityRelativeMovePacket(const std::shared_ptr<Entity>& entity, short deltaX, short deltaY, short deltaZ) {
    std::vector<uint8_t> packetData;
    packetData.push_back(UPDATE_ENTITY_POSITION);

    // Entity ID (VarInt)
    writeVarInt(packetData, entity->entityID);

    writeShort(packetData, deltaX);
    writeShort(packetData, deltaY);
    writeShort(packetData, deltaZ);

    // On Ground (Boolean)
    packetData.push_back(entity->onGround ? 0x01 : 0x00);

    // Broadcast to all other clients
    broadcastToOthers(packetData);
}

void sendPlayerRelativeMovePacket(const std::shared_ptr<Player>& player, short deltaX, short deltaY, short deltaZ) {
    std::vector<uint8_t> packetData;
    packetData.push_back(UPDATE_ENTITY_POSITION);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    writeShort(packetData, deltaX);
    writeShort(packetData, deltaY);
    writeShort(packetData, deltaZ);

    // On Ground (Boolean)
    packetData.push_back(player->onGround ? 0x01 : 0x00);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendEntityLookAndRelativeMovePacket(const std::shared_ptr<Player>& player, short deltaX, short deltaY, short deltaZ, float yaw, float pitch) {
    std::vector<uint8_t> packetData;
    packetData.push_back(UPDATE_ENTITY_POSITION_AND_ROTATION);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    writeShort(packetData, deltaX);
    writeShort(packetData, deltaY);
    writeShort(packetData, deltaZ);

    // Yaw, Pitch (Byte)
    auto yawByte = static_cast<uint8_t>(yaw / (360.0f / 256.0f));
    auto pitchByte = static_cast<uint8_t>(pitch / (360.0f / 256.0f));

    packetData.push_back(yawByte);
    packetData.push_back(pitchByte);

    // On Ground (Boolean)
    packetData.push_back(player->onGround ? 0x01 : 0x00);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendEntityRotationPacket(const std::shared_ptr<Player>& player) {
    std::vector<uint8_t> packetData;
    packetData.push_back(UPDATE_ENTITY_ROTATION);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    // Yaw, Pitch (Byte)
    auto yawByte = static_cast<uint8_t>(player->rotation.yaw / (360.0f / 256.0f));
    auto pitchByte = static_cast<uint8_t>(player->rotation.pitch / (360.0f / 256.0f));

    packetData.push_back(yawByte);
    packetData.push_back(pitchByte);

    // On Ground (Boolean)
    packetData.push_back(player->onGround ? 0x01 : 0x00);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendHeadRotationPacket(const std::shared_ptr<Player>& player) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SET_HEAD_ROTATION);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    // Head Yaw (Byte)
    auto headYawByte = static_cast<uint8_t>(player->rotation.headYaw / (360.0f / 256.0f));
    packetData.push_back(headYawByte);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendEntityTeleportPacket(const std::shared_ptr<Player>& player) {
    std::vector<uint8_t> packetData;
    packetData.push_back(TELEPORT_ENTITY);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    // X, Y, Z (Double)
    writeDouble(packetData, player->position.x);
    writeDouble(packetData, player->position.y);
    writeDouble(packetData, player->position.z);

    // Yaw, Pitch (Byte)
    auto yawByte = static_cast<uint8_t>(player->rotation.yaw / (360.0f / 256.0f));
    auto pitchByte = static_cast<uint8_t>(player->rotation.pitch / (360.0f / 256.0f));

    packetData.push_back(yawByte);
    packetData.push_back(pitchByte);

    // On Ground (Boolean)
    packetData.push_back(player->onGround ? 0x01 : 0x00);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendSpawnEntityPacket(const std::shared_ptr<Entity>& entity) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SPAWN_ENTITY);

    // Entity ID (VarInt)
    writeVarInt(packetData, entity->entityID);

    // Entity UUID (UUID - 16 bytes)
    packetData.insert(packetData.end(), entity->uuid.begin(), entity->uuid.end());

    // Type (VarInt) - Entity type ID from minecraft:entity_type registry
    writeVarInt(packetData, static_cast<int32_t>(entity->type));

    // X, Y, Z (Double)
    writeDouble(packetData, entity->getPositionX());
    writeDouble(packetData, entity->getPositionY());
    writeDouble(packetData, entity->getPositionZ());

    // Pitch, Yaw, Head Yaw (Angles as bytes)
    auto pitch = static_cast<uint8_t>(entity->rotation.pitch / (360.0f / 256.0f));
    auto yaw = static_cast<uint8_t>(entity->rotation.yaw / (360.0f / 256.0f));
    auto headYaw = static_cast<uint8_t>(entity->rotation.headYaw / (360.0f / 256.0f));

    packetData.push_back(pitch);
    packetData.push_back(yaw);
    packetData.push_back(headYaw);

    // Data - Additional data depending on entity type
    std::vector<uint8_t> additionalData;
    entity->serializeAdditionalData(additionalData);
    writeVarInt(packetData, static_cast<int32_t>(additionalData.size()));
    packetData.insert(packetData.end(), additionalData.begin(), additionalData.end());

    // Velocity (Fixed-point, scaled by 8000)
    auto fixedMotionX = static_cast<int16_t>(entity->getMotionX() * 8000);
    auto fixedMotionY = static_cast<int16_t>(entity->getMotionY() * 8000);
    auto fixedMotionZ = static_cast<int16_t>(entity->getMotionZ() * 8000);
    writeShort(packetData, fixedMotionX);
    writeShort(packetData, fixedMotionY);
    writeShort(packetData, fixedMotionZ);

    // Build and send the packet with length prefix
    broadcastToOthers(packetData);
}

void sendSpawnEntityPacket(ClientConnection& client, const std::shared_ptr<Entity>& entity) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SPAWN_ENTITY);

    // Entity ID (VarInt)
    writeVarInt(packetData, entity->entityID);

    // Entity UUID (UUID - 16 bytes)
    packetData.insert(packetData.end(), entity->uuid.begin(), entity->uuid.end());

    // Type (VarInt) - Entity type ID from minecraft:entity_type registry
    writeVarInt(packetData, static_cast<int32_t>(entity->type));

    // X, Y, Z (Double)
    writeDouble(packetData, entity->position.x);
    writeDouble(packetData, entity->position.y);
    writeDouble(packetData, entity->position.z);

    // Pitch, Yaw, Head Yaw (Angles as bytes)
    auto pitch = static_cast<uint8_t>(entity->rotation.pitch / (360.0f / 256.0f));
    auto yaw = static_cast<uint8_t>(entity->rotation.yaw / (360.0f / 256.0f));
    auto headYaw = static_cast<uint8_t>(entity->rotation.headYaw / (360.0f / 256.0f));

    packetData.push_back(pitch);
    packetData.push_back(yaw);
    packetData.push_back(headYaw);

    // Data (VarInt) - Additional data depending on entity type
    std::vector<uint8_t> additionalData;
    entity->serializeAdditionalData(additionalData);
    writeVarInt(packetData, static_cast<int32_t>(additionalData.size()));
    packetData.insert(packetData.end(), additionalData.begin(), additionalData.end());

    // Velocity X, Y, Z (Short) - set to 0 for now
    writeShort(packetData, 0); // Velocity X
    writeShort(packetData, 0); // Velocity Y
    writeShort(packetData, 0); // Velocity Z

    // Build and send the packet with length prefix
    sendPacket(client, packetData);
}

void sendEntityEventPacket(ClientConnection& client, int32_t entityID, uint8_t entityStatus) {
    std::vector<uint8_t> packetData;
    packetData.push_back(ENTITY_EVENT);

    // Entity ID (VarInt)
    writeInt(packetData, entityID);

    // Event ID (Byte)
    writeByte(packetData, entityStatus);

    // Build and send the packet
    sendPacket(client, packetData);
}

void sendPlayerInfoUpdate(ClientConnection& targetClient, const std::vector<std::shared_ptr<Player>>& playersToUpdate, uint8_t actions) {
    std::vector<uint8_t> packetData = { };
    packetData.push_back(PLAYER_INFO_UPDATE);

    // Actions Byte
    packetData.push_back(actions);

    // Number Of Players (VarInt)
    writeVarInt(packetData, static_cast<int32_t>(playersToUpdate.size()));

    for (const auto& player : playersToUpdate) {
        // Append UUID (16 bytes)
        packetData.insert(packetData.end(), player->uuid.begin(), player->uuid.end());

        // Player Actions based on the actions byte
        if (actions & 0x01) { // Add Player
            // Player Name (String)
            writeString(packetData, player->name);

            // Number Of Properties (VarInt)
            writeVarInt(packetData, static_cast<int32_t>(player->properties.size()));

            // Properties
            for (const auto& prop : player->properties) {
                // Property Name (String)
                writeString(packetData, prop.first);
                // Property Value (String)
                writeString(packetData, prop.second.first);
                // Is Signed (Boolean)
                packetData.push_back(0x01); // true
                // Property Signature (String)
                writeString(packetData, prop.second.second);
            }
        }

        if (actions & 0x02) { // Initialize Chat
            // Has Signature Data (Boolean)
            writeByte(packetData, serverConfig.enableSecureChat);

            if (serverConfig.enableSecureChat) {
                if (player->sessionId.size() != 16) {
                    logMessage("Invalid Session ID size for player: " + player->name, LOG_ERROR);
                    disconnectClient(player, "Invalid Session ID size", true);
                    return;
                }

                if (player->sessionKey.pubKey.size() > 512) {
                    logMessage("Public Key size exceeds 512 bytes for player: " + player->name, LOG_ERROR);
                    disconnectClient(player, "Public Key size exceeds 512 bytes", true);
                    return;
                }

                if (player->sessionKey.keySig.size() > 4096) {
                    logMessage("Public Key Signature size exceeds 4096 bytes for player: " + player->name, LOG_ERROR);
                    disconnectClient(player, "Public Key Signature size exceeds 4096 bytes", true);
                    return;
                }
                // Chat Session ID (UUID)
                writeBytes(packetData, player->sessionId);

                // Public key expiry time (Long)
                writeLong(packetData, player->sessionKey.expiresAt);

                // Encoded public key size (VarInt)
                writeVarInt(packetData, static_cast<int32_t>(player->sessionKey.pubKey.size()));

                // Encoded public key
                writeBytes(packetData, player->sessionKey.pubKey);

                // Public key signature size (VarInt)
                writeVarInt(packetData, static_cast<int32_t>(player->sessionKey.keySig.size()));

                // Public key signature
                writeBytes(packetData, player->sessionKey.keySig);
            }
        }

        if (actions & 0x04) { // Update Game Mode
            // Game Mode (VarInt)
            writeVarInt(packetData, player->gameMode);
        }

        if (actions & 0x08) { // Update Listed
            // Listed (Boolean)
            packetData.push_back(player->listed ? 0x01 : 0x00);
        }

        if (actions & 0x10) { // Update Latency
            // Ping (VarInt)
            writeVarInt(packetData, player->ping);
        }

        if (actions & 0x20) { // Update Display Name
            // Has Display Name (Boolean)
            bool hasDisplayName = false; // TODO: Set to true if display name is present. For now, always false.
            packetData.push_back(hasDisplayName ? 0x01 : 0x00);

            if (hasDisplayName) {
                // Display Name (Text Component)
                // For now, skip implementing Text Components
            }
        }
    }

    // Send the packet to the target client
    sendPacket(targetClient, packetData);
}

void sendGameEventPacket(ClientConnection& targetClient, GameEvent event, float value) {
    std::vector<uint8_t> packetData;
    packetData.push_back(GAME_EVENT);

    // Event (Unsigned Byte)
    packetData.push_back(static_cast<uint8_t>(event));

    // Value (Float)
    writeFloat(packetData, value);

    // Send the packet
    sendPacket(targetClient, packetData);
}

void sendGameEvent(GameEvent event, float value) {
    std::vector<uint8_t> packetData;
    packetData.push_back(GAME_EVENT);

    // Event (Unsigned Byte)
    packetData.push_back(static_cast<uint8_t>(event));

    // Value (Float)
    writeFloat(packetData, value);

    // Broadcast to all clients
    broadcastToOthers(packetData);
}

void sendChangeGamemode(ClientConnection& client, const std::shared_ptr<Player>& player, Gamemode gameMode) {
    sendGameEventPacket(client, GameEvent::ChangeGameMode, static_cast<float>(gameMode));
    sendPlayerInfoUpdate(client, { player }, 0x04);
    }

void sendRemoveEntitiesPacket(const std::vector<int32_t>& entityIDs) {
    std::vector<uint8_t> packetData;
    packetData.push_back(REMOVE_ENTITIES);

    // Number of Entities (VarInt)
    writeVarInt(packetData, static_cast<int32_t>(entityIDs.size()));

    // Entity IDs (VarInt)
    for (int32_t entityID : entityIDs) {
        writeVarInt(packetData, entityID);
    }

    // Build and send the packet to all clients
    broadcastToOthers(packetData);
}

void sendTranslatedChatMessage(const std::string& key, const bool actionBar, const std::string& color, const std::vector<std::shared_ptr<Player>>* players, bool log, const std::vector<std::string>* args) {
    std::vector<uint8_t> packetData = { };

    if (players == nullptr) {
        for (const auto &player: globalPlayers | std::views::values) {
            packetData.clear();
            packetData.push_back(SYSTEM_CHAT_MESSAGE);

            nbt::tag_compound textCompound = createTextComponent(getTranslation(key, player->lang, *args), color);
            std::vector<uint8_t> textData = serializeNBT(textCompound, true);
            packetData.insert(packetData.end(), textData.begin(), textData.end());

            writeByte(packetData, actionBar);
            sendPacket(*player->client, packetData);
        }
    }
    else {
        for (const auto& player : *players) {
            if (player->client != nullptr) {
                packetData.clear();
                packetData.push_back(SYSTEM_CHAT_MESSAGE);

                nbt::tag_compound textCompound = createTextComponent(getTranslation(key, player->lang, *args), color);
                std::vector<uint8_t> textData = serializeNBT(textCompound, true);
                packetData.insert(packetData.end(), textData.begin(), textData.end());

                writeByte(packetData, actionBar);
                sendPacket(*player->client, packetData);
            }
        }
    }
    if (log) {
        logMessage(getTranslation(key, consoleLang, *args), LOG_RAW);
    }
}

void sendChatMessage(const std::string& message, const bool actionBar, const std::string& color, const std::vector<std::shared_ptr<Player>>* players, bool log) {
    std::vector<uint8_t> packetData = { };
    packetData.push_back(SYSTEM_CHAT_MESSAGE);

    nbt::tag_compound textCompound = createTextComponent(message, color);
    std::vector<uint8_t> textData = serializeNBT(textCompound, true);
    packetData.insert(packetData.end(), textData.begin(), textData.end());

    writeByte(packetData, actionBar);

    if (players == nullptr) {
        broadcastToOthers(packetData, "");
    }
    else {
        for (const auto& player : *players) {
            if (player->client != nullptr) {
                sendPacket(*player->client, packetData);
            }
        }
    }
    if (log) {
        logMessage(message, LOG_RAW);
    }
}

void sendChatMessage(const std::string& message, const bool actionBar, const std::string& color, const std::shared_ptr<Player>& player, bool log) {
    const std::vector<std::shared_ptr<Player>> singlePlayerList = { player };

    // Call the multiple-players version with a pointer to the single-player vector
    sendChatMessage(message, actionBar, color, &singlePlayerList, log);
}

void sendSetCenterChunkPacket(ClientConnection& targetClient, int32_t chunkX, int32_t chunkZ) {
    std::vector<uint8_t> packetData = { };
    packetData.push_back(SET_CENTER_CHUNK);

    // Serialize Chunk X (VarInt)
    writeVarInt(packetData, chunkX);

    // Serialize Chunk Z (VarInt)
    writeVarInt(packetData, chunkZ);

    // Send the packet to the target client
   sendPacket(targetClient, packetData);
}

void sendResourcePacks(ClientConnection& client) {
    for (const auto& pack : serverConfig.resourcePacks) {
        std::vector<uint8_t> packetData;
        writeVarInt(packetData, ADD_RESOURCE_PACK_PLAY);

        // Resource Pack Push Fields
        std::array<uint8_t, 16> uuid = stringUUIDToBytes(pack.uuid);
        packetData.insert(packetData.end(), uuid.begin(), uuid.end()); // UUID as bytes
        writeString(packetData, pack.url);  // URL to resource pack
        writeString(packetData, pack.hash); // SHA-1 hash
        writeByte(packetData, pack.forced ? 0x01 : 0x00); // Forced

        // Has Prompt Message
        writeByte(packetData, pack.hasPromptMessage ? 0x01 : 0x00);
        if (pack.hasPromptMessage) {
            // Serialize the prompt message as a JSON text component
            nlohmann::json promptJson = {
                {"text", pack.promptMessage}
            };
            std::string promptStr = promptJson.dump();
            writeString(packetData, promptStr);
        }

        // Send the packet
        sendPacket(client, packetData);
    }
}

void sendRemoveResourcePacks(ClientConnection& client, const std::vector<std::string>& uuidsToRemove) {
    std::vector<uint8_t> packetData;
    writeVarInt(packetData, REMOVE_RESOURCE_PACK_CONFIG);

    if (uuidsToRemove.empty()) {
        // Remove all resource packs
        writeByte(packetData, 0x00); // Has UUID = false
    } else {
        // Remove specific resource packs
        writeByte(packetData, 0x01); // Has UUID = true
        for (const auto& uuid : uuidsToRemove) {
            writeString(packetData, uuid); // UUID as string
        }
    }

    // Send the packet
    sendPacket(client, packetData);
}

bool sendKeepAlivePacket(ClientConnection& client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(KEEP_ALIVE_PLAY);

    // Keep Alive ID (Long)
    int64_t keepAliveID = std::chrono::system_clock::now().time_since_epoch().count();
    writeLong(packetData, keepAliveID);
    client.keepAliveID = keepAliveID;

    // Build and send the packet
    return sendPacket(client, packetData);
}

void sendEntityMetadataPacket(const std::vector<MetadataEntry>& metadataEntries, int32_t entityID) {
    std::vector<uint8_t> packetData = { };

    // Packet ID for Entity Metadata
    packetData.push_back(SET_ENTITY_METADATA);

    // Entity ID (VarInt)
    writeVarInt(packetData, entityID);

    // Add each Metadata Entry
    for (const auto& entry : metadataEntries) {
        // Index (Unsigned Byte)
        packetData.push_back(entry.index);

        // Type (VarInt Enum)
        writeVarInt(packetData, static_cast<int32_t>(entry.type));

        // Value (Varies based on type)
        packetData.insert(packetData.end(), entry.value.begin(), entry.value.end());
    }

    // Terminating Entry (0xFF)
    packetData.push_back(0xFF);

    // Broadcast the packet to all other clients
    broadcastToOthers(packetData);
}

void sendEntityMetadataPacket(const std::shared_ptr<Player>& player, const std::vector<MetadataEntry>& metadataEntries, int32_t entityID) {
    std::vector<uint8_t> packetData = { };

    // Packet ID for Entity Metadata
    packetData.push_back(SET_ENTITY_METADATA);

    // Entity ID (VarInt)
    writeVarInt(packetData, entityID);

    // Add each Metadata Entry
    for (const auto& entry : metadataEntries) {
        // Index (Unsigned Byte)
        packetData.push_back(entry.index);

        // Type (VarInt Enum)
        writeVarInt(packetData, static_cast<int32_t>(entry.type));

        // Value (Varies based on type)
        packetData.insert(packetData.end(), entry.value.begin(), entry.value.end());
    }

    // Terminating Entry (0xFF)
    packetData.push_back(0xFF);

    // Broadcast the packet to all other clients except the initiating player
    broadcastToOthers(packetData, player->uuidString);
}

void sendEntityAnimation(const std::shared_ptr<Player> & player, EntityAnimation animation) {
    std::vector<uint8_t> packetData;
    packetData.push_back(ENTITY_ANIMATION);

    // Entity ID (VarInt)
    writeVarInt(packetData, player->entityID);

    // Animation ID (Unsigned Byte)
    packetData.push_back(static_cast<uint8_t>(animation));

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void sendAcknowledgeBlockChange(ClientConnection& client, size_t sequenceID) {
    std::vector<uint8_t> packetData;
    packetData.push_back(ACKNOWLEDGE_BLOCK_CHANGE);

    // Sequence ID (VarInt)
    writeVarInt(packetData, static_cast<int32_t>(sequenceID));

    // Send the packet
    sendPacket(client, packetData);
}

void sendEquipmentPacket(const std::shared_ptr<Player> & player, int32_t entityID, const EquipmentSlot& slot) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SET_EQUIPMENT);

    // Entity ID (VarInt)
    writeVarInt(packetData, entityID);

    // Slot (Byte Enum)
    writeByte(packetData, slot.slotId);

    // Item (Slot)
    writeSlotSimple(packetData, slot.slotData);

    // Broadcast to all other clients
    broadcastToOthers(packetData, player->uuidString);
}

void broadcastPlayerChatMessage(const std::shared_ptr<Player>& sender, const std::string& message, long timestamp, long salt, const std::vector<uint8_t>* signature, const RegistryManager& registryManager, const std::string& chatTypeIdentifier, const std::string& targetName) {
    std::vector<uint8_t> packetData;
    packetData.push_back(PLAYER_CHAT_MESSAGE);

    // Sender UUID
    packetData.insert(packetData.end(), sender->uuid.begin(), sender->uuid.end());

    // Index (VarInt) - set to 0 for now
    writeVarInt(packetData, 0);

    // Message Signature Present
    packetData.push_back((signature && serverConfig.enableSecureChat) ? 0x01 : 0x00);

    // Message Signature
    if (signature && serverConfig.enableSecureChat) {
        packetData.insert(packetData.end(), signature->begin(), signature->end());
    }

    // Body
    writeString(packetData, message);

    // Timestamp
    writeLong(packetData, timestamp);

    // Salt
    writeLong(packetData, salt);

    // Previous Messages
    writeVarInt(packetData, 0); // No previous messages

    // Signature for previous messages (omitted since count is 0)

    // Other
    writeByte(packetData, 0x00); // Unsigned Content Present (false)

    writeVarInt(packetData, 0); // PASS_THROUGH: Message is not filtered at all

    // Chat Formatting (VarInt referencing chat_type registry)
    std::optional<int32_t> chatTypeIndexOpt = registryManager.getRegistryID("minecraft:chat_type", chatTypeIdentifier);
    if (!chatTypeIndexOpt.has_value()) {
        logMessage("Chat type identifier not found in registry: " + chatTypeIdentifier, LOG_ERROR);
        return;
    }
    int32_t chatTypeIndex = chatTypeIndexOpt.value();

    writeVarInt(packetData, chatTypeIndex + 1);

    // Sender Name
    nbt::tag_compound senderNameTag = createTextComponent(sender->name, "white"); // Simple white text
    std::vector<uint8_t> serializedSenderName = serializeNBT(senderNameTag, true);
    writeBytes(packetData, serializedSenderName);

    // Target Name
    if (targetName.empty()) {
        writeByte(packetData, 0x00); // No target name
    } else {
        writeByte(packetData, 0x01); // Target name present
        nbt::tag_compound targetNameTag = createTextComponent(targetName, "white"); // Simple white text
        std::vector<uint8_t> serializedTargetName = serializeNBT(targetNameTag, true);
        writeBytes(packetData, serializedTargetName);
    }

    // Broadcast to all players
    {
        std::lock_guard lock(connectedClientsMutex);
        for (auto &client: connectedClients | std::views::values) {
            sendPacket(*client, packetData);
        }
    }
    logMessage("<" + sender->name + "> " + message, LOG_RAW);
}

void sendCommandsPacket(ClientConnection& client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(COMMANDS);

    // Write the Count (number of nodes)
    writeVarInt(packetData, static_cast<int32_t>(commandGraphNumOfNodes));

    // Write the Nodes array
    writeBytes(packetData, serializedCommandGraph.first);

    // Write the Root index
    writeVarInt(packetData, serializedCommandGraph.second);

    sendPacket(client, packetData);
}

void sendFinishConfigurationPacket(ClientConnection& client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(FINISH_CONFIGURATION);

    // No fields in this packet

    // Build and send the packet
    sendPacket(client, packetData);
}

void sendKnownPacksPacket(ClientConnection& client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(CLIENTBOUND_KNOWN_PACKS);
    // For now, only minecraft:core version 1.21
    writeVarInt(packetData, 1); // Number of packs

    writeString(packetData, "minecraft"); // Pack Namespace
    writeString(packetData, "core"); // Pack ID
    writeString(packetData, "1.21"); // Pack Version

    sendPacket(client, packetData);
}

void sendSetCompressionPacket(ClientConnection& client, int32_t threshold) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SET_COMPRESSION);
    writeVarInt(packetData, threshold); // Compression threshold

    sendPacket(client, packetData);
}

void sendDisconnectionPacket(ClientConnection& client, const std::string& reason) {
    std::vector<uint8_t> packetData;
    packetData.push_back(DISCONNECT);

    // Serialize the reason as a JSON text component
    nlohmann::json textComponent = {
        {"text", reason}
    };
    std::string reasonStr = textComponent.dump();
    writeString(packetData, reasonStr);

    sendPacket(client, packetData);
}

void sendServerLinksPacket(ClientConnection & client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(SERVER_LINKS);

    writeVarInt(packetData, static_cast<int32_t>(serverConfig.serverLinks.size()));
    for (const auto& link : serverConfig.serverLinks) {
        // Is built-in
        writeByte(packetData, 0x00); // Not built-in
        createTextComponent(link.label);
        writeBytes(packetData, serializeNBT(createTextComponent(link.label), true));
        writeString(packetData, link.url);
    }

    sendPacket(client, packetData);
}

void sendServerPluginMessages(ClientConnection & client) {
    std::vector<uint8_t> packetData;
    packetData.push_back(CLIENTBOUND_PLUGIN_MESSAGE_CONFIG);

    std::string channel = "minecraft:brand";
    writeString(packetData, channel);

    std::string brand = "MCpp";
    writeString(packetData, brand);

    sendPacket(client, packetData);
}

void sendReInitializeWorldBorder(double x, double z, double size, int64_t speed, int32_t warningBlocks, int32_t warningTime) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, INITIALIZE_WORLD_BORDER);
    double oldDiameter = worldBorder.size;
    worldBorder.updateCenter(x, z);
    worldBorder.updateSize(size);
    worldBorder.updateWarningDistance(warningBlocks);
    worldBorder.updateWarningTime(warningTime);
    worldBorder.reInitialize();

    // Bound To: X (Double) - Center X
    writeDouble(packet, x);

    // Bound To: Z (Double) - Center Z
    writeDouble(packet, z);

    // Old Diameter (Double) - Set to current size (no resizing)
    writeDouble(packet, oldDiameter);

    // New Diameter (Double) - Set to current size (no resizing)
    writeDouble(packet, size);

    // Speed (VarLong) - 0 (no resizing)
    writeVarLong(packet, speed);

    // Portal Teleport Boundary (VarInt)
    writeVarInt(packet, static_cast<int32_t>(worldBorder.portalTeleportBoundary));

    // Warning Blocks (VarInt)
    writeVarInt(packet, warningBlocks);

    // Warning Time (VarInt)
    writeVarInt(packet, warningTime);

    // Send the packet
    broadcastToOthers(packet);
}

void sendInitializeWorldBorder(ClientConnection& client, const WorldBorder& border) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, INITIALIZE_WORLD_BORDER);

    // Bound To: X (Double) - Center X
    writeDouble(packet, border.centerX);

    // Bound To: Z (Double) - Center Z
    writeDouble(packet, border.centerZ);

    // Old Diameter (Double) - Set to current size (no resizing)
    writeDouble(packet, border.size);

    // New Diameter (Double) - Set to current size (no resizing)
    writeDouble(packet, border.size);

    // Speed (VarLong) - 0 (no resizing)
    writeVarLong(packet, 0);

    // Portal Teleport Boundary (VarInt)
    writeVarInt(packet, static_cast<int32_t>(border.portalTeleportBoundary));

    // Warning Blocks (VarInt)
    writeVarInt(packet, border.warningBlocks);

    // Warning Time (VarInt)
    writeVarInt(packet, border.warningTime);

    // Send the packet
    sendPacket(client, packet);
}

void sendTimeUpdatePacket(ClientConnection& client) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, UPDATE_TIME);

    // World Age (Long) - Not changed by server commands
    int64_t currentWorldAge = worldTime.getWorldAge();
    writeLong(packet, currentWorldAge);

    // Time of Day (Long) - Based on world time
    int64_t currentTimeOfDay = worldTime.getTimeOfDay();
    writeLong(packet, currentTimeOfDay);

    // Send the packet
    sendPacket(client, packet);
}

void sendSetBorderCenter(double x, double z) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_BORDER_CENTER);
    worldBorder.updateCenter(x, z);

    // Center X (Double)
    writeDouble(packet, worldBorder.centerX);

    // Center Z (Double)
    writeDouble(packet, worldBorder.centerZ);

    // Send the packet to all clients
    broadcastToOthers(packet);
}

void sendSetBorderLerpSize(double newDiameter, int64_t speed) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_BORDER_LERP_SIZE);
    double oldDiameter = worldBorder.size;
    worldBorder.updateSize(newDiameter);

    // Old Diameter (Double)
    writeDouble(packet, oldDiameter);

    // New Diameter (Double)
    writeDouble(packet, worldBorder.size);

    // Speed (VarLong)
    writeVarLong(packet, speed);

    // Send the packet to all clients
    broadcastToOthers(packet);
}

void sendSetBorderSize(double newDiameter) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_BORDER_SIZE);
    worldBorder.updateSize(newDiameter);

    // New Diameter (Double)
    writeDouble(packet, worldBorder.size);

    // Send the packet to all clients
    broadcastToOthers(packet);
}

void sendSetBorderWarningDelay(int32_t warningTime) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_BORDER_WARNING_DELAY);
    worldBorder.updateWarningTime(warningTime);

    // Warning Time (VarInt)
    writeVarInt(packet, worldBorder.warningTime);

    // Send the packet to all clients
    broadcastToOthers(packet);
}

void sendSetBorderWarningDistance(int32_t warningBlocks) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_BORDER_WARNING_DISTANCE);
    worldBorder.updateWarningDistance(warningBlocks);

    // Warning Blocks (VarInt)
    writeVarInt(packet, worldBorder.warningBlocks);

    // Send the packet to all clients
    broadcastToOthers(packet);
}

void sendBossbar(Bossbar& bossbar, int32_t action) {
    std::vector<uint8_t> packet;
    writeByte(packet, BOSS_BAR);
    std::array<uint8_t, 16> uuid = bossbar.getUUID();
    std::vector<uint8_t> uuidBytes;
    uuidBytes.insert(uuidBytes.end(), uuid.begin(), uuid.end());
    writeBytes(packet, uuidBytes);
    writeVarInt(packet, action);
    switch (action) {
        case 0: { // Add
            // Title (Text Component)
            std::vector<uint8_t> titleBytes = serializeNBT(bossbar.getTitle(), true);
            writeBytes(packet, titleBytes);
            // Health (Float)
            writeFloat(packet, bossbar.getHealth());
            // Color (VarInt)
            writeVarInt(packet, bossbar.getColor());
            // Division (VarInt)
            writeVarInt(packet, bossbar.getDivision());
            // Flags (Unsigned Byte)
            writeByte(packet, bossbar.getFlags());
            break;
        }
        case 1: { // Remove
            // No fields
            break;
        }
        case 2: { // Update Health
            // Health (Float)
            writeFloat(packet, bossbar.getHealth());
            break;
        }
        case 3: { // Update Title
            // Title (Text Component)
            std::vector<uint8_t> titleBytes = serializeNBT(bossbar.getTitle(), true);
            writeBytes(packet, titleBytes);
            break;
        }
        case 4: { // Update Style
            // Color (VarInt)
            writeVarInt(packet, bossbar.getColor());
            // Division (VarInt)
            writeVarInt(packet, bossbar.getDivision());
            break;
        }
        case 5: { // Update flags
            // Flags (Unsigned Byte)
            writeByte(packet, bossbar.getFlags());
            break;
        }
        default: {
            logMessage("Invalid bossbar action: " + std::to_string(action), LOG_ERROR);
            return;
        }
    }
    for (const auto player : bossbar.getPlayers()) {
        if (player->client != nullptr) {
            sendPacket(*player->client, packet);
        }
    }
}

void sendCommandSuggestionsResponse(ClientConnection& client, int32_t transactionID, const std::vector<std::string>& suggestions, int32_t start) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, COMMAND_SUGGESTIONS_RESPONSE);
    writeVarInt(packet, transactionID);
    writeVarInt(packet, start);
    writeVarInt(packet, 0);
    writeVarInt(packet, static_cast<int32_t>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        writeString(packet, suggestion);
        writeByte(packet, 0x00); // No text component
    }
    sendPacket(client, packet);
}

void sendBundleDelimiter() {
    std::vector<uint8_t> packet;
    writeVarInt(packet, BUNDLE_DELIMITER);
    broadcastToOthers(packet);
}

void sendBundleDelimiter(ClientConnection& client) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, BUNDLE_DELIMITER);
    sendPacket(client, packet);
}

void sendEntityVelocity(const std::shared_ptr<Entity>& entity) {
    std::vector<uint8_t> packet;
    writeVarInt(packet, SET_ENTITY_VELOCITY);

    // Entity ID (VarInt)
    writeVarInt(packet, entity->entityID);

    // Velocity X (Short)
    writeShort(packet, static_cast<int16_t>(entity->getMotionX() * 8000));

    // Velocity Y (Short)
    writeShort(packet, static_cast<int16_t>(entity->getMotionY() * 8000));

    // Velocity Z (Short)
    writeShort(packet, static_cast<int16_t>(entity->getMotionZ() * 8000));

    // Broadcast to all clients
    broadcastToOthers(packet);
}

void sendPickUpItem(const std::shared_ptr<Entity>& collectedEntity, const std::shared_ptr<Entity>& collectorEntity, int8_t count) {
    std::vector<uint8_t> packet;
    writeByte(packet, PICK_UP_ITEM);

    // Collected Entity ID (VarInt)
    writeVarInt(packet, collectedEntity->entityID);

    // Collector Entity ID (VarInt)
    writeVarInt(packet, collectorEntity->entityID);

    // Count (VarInt)
    writeVarInt(packet, count);

    // Broadcast to all clients
    broadcastToOthers(packet);
}

void SendSetContainerSlot(ClientConnection& client, const int8_t windowID, const int32_t stateID, const uint16_t slotID, const SlotData& slot) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_CONTAINER_SLOT);

    // Window ID (Byte)
    writeByte(packet, windowID);

    // State ID (VarInt)
    writeVarInt(packet, stateID);

    // Slot ID (Short)
    writeShort(packet, slotID);

    // Slot (Slot)
    writeSlotSimple(packet, slot);

    sendPacket(client, packet);
}

void sendUpdateRecipes(ClientConnection& client) {
    std::vector<uint8_t> packet;
    writeByte(packet, UPDATE_RECIPES);

    /*
    writeVarInt(packet, static_cast<int32_t>(craftingRecipes.size()));
    for (const auto&[key, CraftingRecipe] : craftingRecipes) {
        writeBytes(packet, CraftingRecipe.serialize(key));
    }


    writeVarInt(packet, 1);
    writeBytes(packet, craftingRecipes.find(36)->second.serialize(36));


    sendPacket(client, packet);
    */
}

void sendContainerContent(ClientConnection& client, uint8_t windowID, int32_t stateID, Inventory& inventory) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_CONTAINER_CONTENT);

    writeUByte(packet, windowID);
    writeVarInt(packet, stateID);
    writeVarInt(packet, inventory.size);
    for (int i = 0; i < inventory.size; i++) {
        if (inventory.slots.contains(i)) {
            writeSlotSimple(packet, inventory.slots[i]);
        } else {
            SlotData emptySlot;
            emptySlot.itemCount = 0;
            emptySlot.itemId = 0;
            writeSlotSimple(packet, emptySlot);
        }
    }
    writeSlotSimple(packet, inventory.carriedItem);

    sendPacket(client, packet);
}

void sendOpenScreen(ClientConnection& client, const uint8_t windowID, const uint8_t windowType, const std::string& title) {
    std::vector<uint8_t> packet;
    writeByte(packet, OPEN_SCREEN);

    writeVarInt(packet, windowID);
    writeVarInt(packet, windowType);
    nbt::tag_compound titleTag = createTextComponent(title);
    writeBytes(packet, serializeNBT(titleTag, true));

    sendPacket(client, packet);
}

void sendBlockDestroyStage(const std::shared_ptr<Player>& player, const Position &blockPos, const int8_t stage) {
    std::vector<uint8_t> packet;
    writeByte(packet, BLOCK_DESTROY_STAGE);

    // Entity ID (VarInt)
    writeVarInt(packet, player->entityID);

    // Block Position (Position)
    writeLong(packet, encodePosition(static_cast<int32_t>(blockPos.x), static_cast<int32_t>(blockPos.y), static_cast<int32_t>(blockPos.z)));

    // Destroy Stage (Byte)
    writeByte(packet, stage);

    // Broadcast to all clients // TODO: Only broadcast to clients that have the chunk loaded
    broadcastToOthers(packet, player->uuidString);
}

void sendUpdateAttributes(ClientConnection& client, const int32_t entityID, const std::vector<Attribute>& attributes) {
    std::vector<uint8_t> packet;
    writeByte(packet, UPDATE_ATTRIBUTES);

    // Entity ID (VarInt)
    writeVarInt(packet, entityID);

    // Number of Attributes (VarInt)
    writeVarInt(packet, static_cast<int32_t>(attributes.size()));

    for (const auto& attribute : attributes) {
        // Attribute Id (VarInt)
        writeVarInt(packet, attribute.id);

        // Attribute Value (Double)
        writeDouble(packet, attribute.value);

        // Attribute Modifiers (VarInt)
        writeVarInt(packet, static_cast<int32_t>(/*attribute.modifiers.size()*/0));
        /*
        for (const auto& modifier : attribute.modifiers) {
            // Modifier UUID (UUID)
            packet.insert(packet.end(), modifier.uuid.begin(), modifier.uuid.end());

            // Modifier Amount (Double)
            writeDouble(packet, modifier.amount);

            // Modifier Operation (VarInt)
            writeVarInt(packet, static_cast<int32_t>(modifier.operation));
        }
        */
    }

    sendPacket(client, packet);
}

void sendPlayerAbilities(ClientConnection& client, const uint8_t flags, const float flyingSpeed, const float fovModifier) {
    std::vector<uint8_t> packet;
    writeByte(packet, PLAYER_ABILITIES);

    // Flags (Unsigned Byte)
    writeByte(packet, flags);

    // Flying Speed (Float)
    writeFloat(packet, flyingSpeed);

    // Walking Speed (Float)
    writeFloat(packet, fovModifier);

    sendPacket(client, packet);
}

void sendSetHeldItem(ClientConnection& client, const int8_t slot) {
    std::vector<uint8_t> packet;
    writeByte(packet, SET_HELD_ITEM);

    // Slot (Byte)
    writeByte(packet, slot);

    sendPacket(client, packet);
}

void sendFeatureFlags(ClientConnection& client, const std::vector<std::string>& flags) {
    std::vector<uint8_t> packet;
    writeByte(packet, FEATURE_FLAGS);

    // Flags (Identifier Array)
    writeVarInt(packet, static_cast<int32_t>(flags.size()));
    for (const auto& flag : flags) {
        writeString(packet, flag);
    }

    sendPacket(client, packet);
}
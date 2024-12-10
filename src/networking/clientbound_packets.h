#ifndef CLIENTBOUND_PACKETS_H
#define CLIENTBOUND_PACKETS_H
#include <cstdint>
#include <memory>
#include <vector>

#include "network.h"
#include "packet_ids.h"
#include "core/server.h"
#include "entities/player.h"
#include "enums/enums.h"
#include "utils/translation.h"

struct ClientConnection;
class RegistryManager;
class Bossbar;
struct WorldBorder;
struct EquipmentSlot;
struct MetadataEntry;
enum class GameEvent : uint8_t;
class Entity;
struct Position;

void sendRemoveEntityPacket(const int32_t& entityID);
void sendPlayerInfoRemove(const std::shared_ptr<Player>& player);
void sendRegistryDataPacket(ClientConnection& client, RegistryManager& registryManager);
void sendWorldEventPacket(ClientConnection& client, const int& worldEvent, const Position& position, const int& data);
bool sendUpdateTagsPacket(ClientConnection& client);
void sendJoinGamePacket(ClientConnection& client, int32_t entityID);
void sendSynchronizePlayerPositionPacket(ClientConnection& client, const std::shared_ptr<Player> &player);
void sendEntityRelativeMovePacket(const std::shared_ptr<Entity>& entity, short deltaX, short deltaY, short deltaZ);
void sendPlayerRelativeMovePacket(const std::shared_ptr<Player>& player, short deltaX, short deltaY, short deltaZ);
void sendEntityLookAndRelativeMovePacket(const std::shared_ptr<Player>& player, short deltaX, short deltaY, short deltaZ, float yaw, float pitch);
void sendEntityRotationPacket(const std::shared_ptr<Player>& player);
void sendHeadRotationPacket(const std::shared_ptr<Player>& player);
void sendEntityTeleportPacket(const std::shared_ptr<Player>& player);
void sendSpawnEntityPacket(const std::shared_ptr<Entity>& entity);
void sendSpawnEntityPacket(ClientConnection& client, const std::shared_ptr<Entity>& entity);
void sendEntityEventPacket(ClientConnection& client, int32_t entityID, uint8_t entityStatus);
void sendPlayerInfoUpdate(ClientConnection& targetClient, const std::vector<std::shared_ptr<Player>>& playersToUpdate, uint8_t actions);
void sendGameEventPacket(ClientConnection& targetClient, GameEvent event, float value);
void sendGameEvent(GameEvent event, float value);
void sendChangeGamemode(ClientConnection& client, const std::shared_ptr<Player>& player, Gamemode gameMode);
void sendRemoveEntitiesPacket(const std::vector<int32_t>& entityIDs);
void sendTranslatedChatMessage(const std::string& key, bool actionBar = false, const std::string& color = "white", const std::vector<std::shared_ptr<Player>>* players = nullptr, bool log = true, const std::vector<std::string>* args = nullptr);
void sendChatMessage(const std::string& message, bool actionBar, const std::string& color, const std::shared_ptr<Player>& player, bool log = true);
void sendChatMessage(const std::string& message, bool actionBar = false, const std::string& color = "white", const std::vector<std::shared_ptr<Player>>* players = nullptr, bool log = true);
void sendEntityEventPacket(ClientConnection& client, int32_t entityID, uint8_t entityStatus);
void sendChangeGamemode(ClientConnection& client, const std::shared_ptr<Player>& player, Gamemode gameMode);
void sendDisconnectionPacket(ClientConnection& client, const std::string& reason);
void sendSetCenterChunkPacket(ClientConnection& targetClient, int32_t chunkX, int32_t chunkZ);
void sendResourcePacks(ClientConnection& client);
void sendRemoveResourcePacks(ClientConnection& client, const std::vector<std::string>& uuidsToRemove = {});
bool sendKeepAlivePacket(ClientConnection& client);
void sendEntityMetadataPacket(const std::vector<MetadataEntry>& metadataEntries, int32_t entityID);
void sendEntityMetadataPacket(const std::shared_ptr<Player>& player, const std::vector<MetadataEntry>& metadataEntries, int32_t entityID);
void sendEntityAnimation(const std::shared_ptr<Player> & player, EntityAnimation animation);
void sendAcknowledgeBlockChange(ClientConnection& client, size_t sequenceID);
void sendEquipmentPacket(const std::shared_ptr<Player> & player, int32_t entityID, const EquipmentSlot& slot);
void broadcastPlayerChatMessage(const std::shared_ptr<Player>& sender, const std::string& message, long timestamp, long salt, const std::vector<uint8_t>* signature, const RegistryManager& registryManager, const std::string& chatTypeIdentifier = "minecraft:chat", const std::string& targetName = "");
void sendCommandsPacket(ClientConnection& client);
void sendFinishConfigurationPacket(ClientConnection& client);
void sendKnownPacksPacket(ClientConnection& client);
void sendSetCompressionPacket(ClientConnection& client, int32_t threshold);
void sendServerLinksPacket(ClientConnection & client);
void sendServerPluginMessages(ClientConnection & client);
void sendReInitializeWorldBorder(double x, double z, double size, int64_t speed, int32_t warningBlocks, int32_t warningTime);
void sendInitializeWorldBorder(ClientConnection& client, const WorldBorder& border);
void sendTimeUpdatePacket(ClientConnection& client);
void sendSetBorderCenter(double x, double z);
void sendSetBorderLerpSize(double newDiameter, int64_t speed);
void sendSetBorderSize(double newDiameter);
void sendSetBorderWarningDelay(int32_t warningTime);
void sendSetBorderWarningDistance(int32_t warningBlocks);
void sendBossbar(Bossbar& bossbar, int32_t action);
void sendCommandSuggestionsResponse(ClientConnection& client, int32_t transactionID, const std::vector<std::string>& suggestions, int32_t start);
void sendBundleDelimiter(ClientConnection& client);
void sendBundleDelimiter();
void sendEntityVelocity(const std::shared_ptr<Entity>& entity);
void sendPickUpItem(const std::shared_ptr<Entity>& collectedEntity, const std::shared_ptr<Entity>& collectorEntity, int8_t count);
void SendSetContainerSlot(ClientConnection& client, int8_t windowID, int32_t stateID, uint16_t slotID, const SlotData& slot);
void sendUpdateRecipes(ClientConnection& client);
void sendContainerContent(ClientConnection& client, uint8_t windowID, int32_t stateID, Inventory& inventory);
void sendOpenScreen(ClientConnection& client, uint8_t windowID, uint8_t windowType, const std::string& title);
void sendBlockDestroyStage(const std::shared_ptr<Player>& player, const Position &blockPos, int8_t stage);
void sendUpdateAttributes(ClientConnection& client, int32_t entityID, const std::vector<Attribute>& attributes);
void sendPlayerAbilities(ClientConnection& client, uint8_t flags, float flyingSpeed, float fovModifier);
void sendSetHeldItem(ClientConnection& client, int8_t slot);
void sendFeatureFlags(ClientConnection& client, const std::vector<std::string>& flags);

template<typename... Args>
void sendTranslatedChatMessage(const std::string& key, const bool actionBar = false, const std::string& color = "white", const std::vector<std::shared_ptr<Player>>* players = nullptr, bool log = true, Args&&... args) {
    std::vector<uint8_t> packetData = { };

    if (players == nullptr) {
        for (const auto &player: globalPlayers | std::views::values) {
            packetData.clear();
            packetData.push_back(SYSTEM_CHAT_MESSAGE);

            nbt::tag_compound textCompound = createTextComponent(getTranslation(key, player->lang, std::forward<Args>(args)...), color);
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

                nbt::tag_compound textCompound = createTextComponent(getTranslation(key, player->lang, std::forward<Args>(args)...), color);
                std::vector<uint8_t> textData = serializeNBT(textCompound, true);
                packetData.insert(packetData.end(), textData.begin(), textData.end());

                writeByte(packetData, actionBar);
                sendPacket(*player->client, packetData);
            }
        }
    }
    if (log) {
        logMessage(getTranslation(key, consoleLang, std::forward<Args>(args)...), LOG_RAW);
    }
}

#endif //CLIENTBOUND_PACKETS_H

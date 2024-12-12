#ifndef SERVER_H
#define SERVER_H
#include <mutex>
#include <string>
#include <unordered_map>

#include "data/crafting_recipes.h"
#include "data/data.h"
#include "entities/entity.h"
#include "entities/entity_manager.h"
#include "world/flatworld.h"
#include "server/rcon_server.h"
#include "utils/thread_pool.h"
#include "world/boss_bar.h"
#include "world/weather.h"
#include "world/world_border.h"
#include "world/world_time.h"

#define PRINT_DEBUG 1
constexpr double GRAVITY = 0.04;

class RootNode;
struct ClientConnection;

void runServer();

enum class GameEvent : uint8_t {
    NoRespawnBlockAvailable = 0,
    BeginRaining = 1,
    EndRaining = 2,
    ChangeGameMode = 3,
    WinGame = 4,
    DemoEvent = 5,
    ArrowHitPlayer = 6,
    RainLevelChange = 7,
    ThunderLevelChange = 8,
    PlayPufferfishStingSound = 9,
    PlayElderGuardianAppearance = 10,
    EnableRespawnScreen = 11,
    LimitedCrafting = 12,
    StartWaitingForLevelChunks = 13
};

// Spawn position of the world
inline Position spawnPosition = {0, 0, 0};

inline std::unordered_map<std::string, FlatWorldSettings> flatWorldPresets = loadFlatWorldPresets(
            "../resources/flatworld_presets.json");

inline EntityManager entityManager;

inline std::unordered_map<std::string, BiomeData> biomes;
inline std::unordered_map<std::string, BlockData> blocks;
inline std::unordered_map<std::string, std::vector<int>> blockTags;
inline std::unordered_map<std::string, ItemData> items;
inline std::unordered_map<int, ItemData> itemIDs;
inline std::unordered_map<std::string, std::vector<int>> itemTags;
inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>> translations;
inline std::unordered_multimap<uint16_t, CraftingRecipe> craftingRecipes;

inline std::shared_ptr<RootNode> globalCommandGraph;
inline size_t commandGraphNumOfNodes;
inline std::pair<std::vector<uint8_t>, int> serializedCommandGraph;

inline std::unordered_map<std::string, std::shared_ptr<Player>> globalPlayers; // Key: UUID
inline std::unordered_map<std::string, std::shared_ptr<Player>> globalPlayersName; // Key: Username
inline std::mutex playersMutex;

inline std::thread miningThread;
inline std::atomic<bool> miningRunning;

inline std::unordered_map<std::string, ClientConnection*> connectedClients;
inline std::mutex connectedClientsMutex;

inline thread_pool threadPool(std::thread::hardware_concurrency());

inline std::unique_ptr<RCONServer> rconServer;

// TODO: Create a world class to hold all world data
inline WorldBorder worldBorder;
inline WorldTime worldTime;
inline std::unordered_map<std::array<uint8_t, 16>, Bossbar, ArrayHash, ArrayEqual> bossBars;
inline Weather weather;

inline std::string consoleLang;

inline std::unordered_map<std::string, std::vector<int>> blockNameToShapeIDs;
inline std::unordered_map<int, std::vector<BoundingBox>> shapeIDToShapes;

#endif // SERVER_H

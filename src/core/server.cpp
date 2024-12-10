#include "server.h"
#include "networking/network.h"
#include "networking/client.h"
#include "config.h"
#include <iostream>
#include <thread>

#include "commands/CommandBuilder.h"
#include "data/crafting_recipes.h"
#include "entities/item_entity.h"
#include "networking/clientbound_packets.h"
#include "server/query_server.h"
#include "server/rcon_server.h"
#include "utils/translation.h"
#include "world/world.h"

void tickingSystem() {
    using namespace std::chrono;
    auto tickCount = 0;

    // Calculate the duration between ticks based on ticksPerSecond
    double ticksPerSecond = static_cast<double>(serverConfig.ticksPerSecond);
    double millisecondsPerTick = 1000.0 / ticksPerSecond;

    // Convert to chrono duration with floating-point precision
    auto tickInterval = duration<double, std::milli>(millisecondsPerTick);

    auto nextTick = steady_clock::now() + tickInterval;

    while (true) {
        // Wait until the next tick
        std::this_thread::sleep_until(nextTick);
        // Increment world time
        worldTime.tick();
        if (tickCount % 20 == 0) {
            // Every second

            // Notify all connected clients about the updated time
            {
                std::lock_guard lock(connectedClientsMutex);
                for (auto &existingClient: connectedClients | std::views::values) {
                    sendTimeUpdatePacket(*existingClient);
                }
            }
        }

        // Update weather
        weather.handleTick();
        std::unordered_map<int32_t, std::shared_ptr<Entity>> entities = entityManager.getAllEntities();
        for (auto &entity: entityManager.getAllEntities() | std::views::values) {
            if (entity->type != EntityType::Item) {
                continue; // Only process item entities
            }

            auto item = std::static_pointer_cast<Item>(entity);

            if (item->getCooldown() > 0) {
                item->setCooldown(item->getCooldown() - 1);
            }

            double newMotionY{};
            {
                // Parameters for velocity calculation
                double initialVelocityY = item->getMotionY();
                double dragY = item->getDragY();
                double acceleration = GRAVITY; // Gravity acts as acceleration
                int ticksPassed = 1;
                DragApplicationOrder order = AfterAcceleration;

                // Update velocity with gravity
                newMotionY = calculateFinalVelocity(initialVelocityY, dragY, acceleration, ticksPassed, order) - acceleration;
                item->setMotion(item->getMotionX(), newMotionY, item->getMotionZ());
            }

            // Calculate potential new position
            double potentialPosX = item->getPositionX() + item->getMotionX();
            double potentialPosY = item->getPositionY() + item->getMotionY();
            double potentialPosZ = item->getPositionZ() + item->getMotionZ();

            // --- Collision Detection for Y-axis ---
            // Temporarily set new position for collision check
            Item tempItemY = *item;
            double oldPosY = item->getPositionY();
            tempItemY.setPosition(item->getPositionX(), potentialPosY, item->getPositionZ());

            // Check for collision
            BoundingBox collidedBlockBoxY;
            bool hasCollidedY = checkCollision(tempItemY, collidedBlockBoxY, Axis::Y);

            if (hasCollidedY) {
                double adjustedPosY;
                if (item->getMotionY() > 0.0) {
                    adjustedPosY = collidedBlockBoxY.minY - 0.25;
                } else {
                    adjustedPosY = collidedBlockBoxY.maxY;
                }

                // Set the item's position to the adjusted position
                item->setPosition(potentialPosX, adjustedPosY, potentialPosZ);

                // Reset Y-velocity to prevent further downward movement
                item->setMotion(item->getMotionX(), 0.0, item->getMotionZ());

                // Set on-ground flag
                item->setOnGround(true);
            } else {
                // No collision; update position normally
                item->setPosition(item->getPositionX(), potentialPosY, item->getPositionZ());
                item->setOnGround(false);
            }

            // --- Collision Detection for X-axis ---
            // Temporarily set new position for collision check
            Item tempItemX = *item;
            double oldPosX = item->getPositionX();
            tempItemX.setPosition(potentialPosX, item->getPositionY(), item->getPositionZ());

            // Check for collision
            BoundingBox collidedBlockBoxX;
            bool hasCollidedX = checkCollision(tempItemX, collidedBlockBoxX, Axis::X);

            if (hasCollidedX) {
                double adjustedPosX;
                if (item->getMotionX() > 0.0) {
                    adjustedPosX = collidedBlockBoxX.minX - 0.125;
                } else {
                    adjustedPosX = collidedBlockBoxX.maxX + 0.125;
                }

                // Set the item's position to the adjusted position
                item->setPosition(adjustedPosX, item->getPositionY(), item->getPositionZ());

                // Reset Y-velocity to prevent further downward movement
                item->setMotion(0.0, item->getMotionY(), item->getMotionZ());

                // Set on-ground flag
                item->setOnGround(true);
            } else {
                // No collision; update position normally
                item->setPosition(potentialPosX, item->getPositionY(), item->getPositionZ());
                item->setOnGround(false);
            }

            // --- Collision Detection for Z-axis ---
            // Temporarily set new position for collision check
            Item tempItemZ = *item;
            double oldPosZ = item->getPositionZ();
            tempItemX.setPosition(item->getPositionX(), item->getPositionY(), potentialPosZ);

            // Check for collision
            BoundingBox collidedBlockBoxZ;
            bool hasCollidedZ = checkCollision(tempItemX, collidedBlockBoxZ, Axis::Z);

            if (hasCollidedZ) {
                double adjustedPosZ;
                if (item->getMotionZ() > 0.0) {
                    adjustedPosZ = collidedBlockBoxZ.minZ - 0.125;
                } else {
                    adjustedPosZ = collidedBlockBoxZ.maxZ + 0.125;
                }

                // Set the item's position to the adjusted position
                item->setPosition(item->getPositionX(), item->getPositionY(), adjustedPosZ);

                // Reset Y-velocity to prevent further downward movement
                item->setMotion(item->getMotionX(), item->getMotionY(), 0.0);

                // Set on-ground flag
                item->setOnGround(true);
            } else {
                // No collision; update position normally
                item->setPosition(item->getPositionX(), item->getPositionY(), potentialPosZ);
                item->setOnGround(false);
            }

            // Apply drag to horizontal motion
            double currentDragX = item->getDragX();

            double newMotionX = calculateFinalVelocity(item->getMotionX(), hasCollidedY ? 0.454 : currentDragX, 0, 1, AfterAcceleration);
            double newMotionZ = calculateFinalVelocity(item->getMotionZ(), hasCollidedY ? 0.454 : currentDragX, 0, 1, AfterAcceleration);

            item->setMotion(newMotionX, item->getMotionY(), newMotionZ);

            // Clamp very small velocities to zero to prevent indefinite drifting
            constexpr double MIN_VELOCITY = 0.001;
            if (std::abs(newMotionX) < MIN_VELOCITY) {
                item->setMotion(0.0, item->getMotionY(), item->getMotionZ());
            }
            if (std::abs(newMotionY) < MIN_VELOCITY) {
                item->setMotion(item->getMotionX(), 0.0, item->getMotionZ());
            }
            if (std::abs(newMotionZ) < MIN_VELOCITY) {
                item->setMotion(item->getMotionX(), item->getMotionY(), 0.0);
            }

            double relativeDeltaX = hasCollidedX ? (item->getPositionX() - oldPosX) : item->getMotionX();
            double relativeDeltaY = hasCollidedY ? (item->getPositionY() - oldPosY) : item->getMotionY();
            double relativeDeltaZ = hasCollidedZ ? (item->getPositionZ() - oldPosZ) : item->getMotionZ();

            auto deltaXShort = static_cast<short>(relativeDeltaX  * 4096.0);
            auto deltaYShort = static_cast<short>(relativeDeltaY * 4096.0);
            auto deltaZShort = static_cast<short>(relativeDeltaZ * 4096.0);

            // Send relative move packet to clients
            sendEntityRelativeMovePacket(item, deltaXShort, deltaYShort, deltaZShort);
            sendEntityVelocity(item);
            // Items crossing a block boundary are processed every 2 ticks
            if (tickCount % 2 == 0 &&
            (static_cast<int32_t>(std::floor(item->getPositionX())) != static_cast<int32_t>(std::floor(oldPosX)) ||
             static_cast<int32_t>(std::floor(item->getPositionZ())) != static_cast<int32_t>(std::floor(oldPosZ))))
            {
                item->tryMerge();
            }
        }

        if (tickCount % 40 == 0) {
            for (auto &entity: entityManager.getAllEntities() | std::views::values) {
                if (entity->type != EntityType::Item) {
                    continue; // Only process item entities
                }
                auto item = std::static_pointer_cast<Item>(entity);
                item->tryMerge();
            }
        }

        // Schedule the next tick
        nextTick += tickInterval;
        tickCount++;
    }
}

void startMiningScheduler() {
    miningRunning = true;
    miningThread = std::thread(miningScheduler, std::ref(globalPlayers), std::ref(miningRunning));
	set_thread_name(miningThread, "miningScheduler");
}

void stopMiningScheduler() {
    miningRunning = false;
    if (miningThread.joinable()) {
        miningThread.join();
    }
}


void runServer() {
    auto startTime = std::chrono::system_clock::now();

    loadConfig();
    manageResourcePacks();
    buildAllCommands();

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    int wsaStartup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaStartup != 0) {
        logMessage("Failed to initialize Winsock.", LOG_ERROR);
        return;
    }
#endif

    SocketType serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSock == INVALID_SOCKET) {
        logMessage("Failed to create socket.", LOG_ERROR);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // Set socket options
    int opt = 1;
#ifdef _WIN32
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
#ifdef _WIN32
    serverAddr.sin_addr.s_addr = INADDR_ANY;
#else
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    serverAddr.sin_port = htons(serverConfig.port);

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        logMessage("Failed to bind socket.", LOG_ERROR);
#ifdef _WIN32
        closesocket(serverSock);
        WSACleanup();
#else
        close(serverSock);
#endif
        return;
    }

    if (listen(serverSock, SOMAXCONN) == SOCKET_ERROR) {
        logMessage("Failed to listen on socket.", LOG_ERROR);
#ifdef _WIN32
        closesocket(serverSock);
        WSACleanup();
#else
        close(serverSock);
#endif
        return;
    }

    // Load world
    auto world = new World("world");
    if (!world->load()) {
        logMessage("Failed to load world.", LOG_ERROR);
    }

    blocks = loadBlocks("../resources/blocks.json");
    biomes = loadBiomes("../resources/biomes.json");
    items = loadItems("../resources/items.json");
    itemIDs = loadItemIDs("../resources/items.json");
    translations = loadTranslations("../resources/languages.json");
    loadCollisions("../resources/blockCollisionShapes.json");
    craftingRecipes = loadCraftingRecipes("../resources/recipes/crafting_recipes.json");
    blockMatTags = getBlockMatTags(blocks);

    auto endTime = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedSeconds = endTime - startTime;
    logMessage(getTranslation("server.start.time", consoleLang, std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsedSeconds).count())), LOG_INFO);
    logMessage(getTranslation("server.start.port", consoleLang, std::to_string(serverConfig.port)), LOG_INFO);

    // Initialize and start the QueryServer if enabled
    std::unique_ptr<QueryServer> queryServer;
    if (serverConfig.enableQuery) {
        queryServer = std::make_unique<QueryServer>(serverConfig.queryPort);
        queryServer->start();
    }

    // Initialize and start the RCON if enabled
    if (serverConfig.enableRcon) {
        rconServer = std::make_unique<RCONServer>(serverConfig.rconPort, serverConfig.rconPassword);
        if (!rconServer->start()) {
            logMessage("Failed to start RCON server.", LOG_ERROR);
        }
    }

    startMiningScheduler();

    // Start the console input thread
    std::thread consoleThread([&]() {
        std::string input;
        while (std::getline(std::cin, input)) {
            if (input.empty()) continue;
            handleConsoleCommand(input);
        }
    });
	set_thread_name(consoleThread, "ConsoleThread");

    // Detach the thread to allow it to run independently
    consoleThread.detach();

    std::thread tickThread(tickingSystem);
	set_thread_name(tickThread, "TickThread");
    tickThread.detach();

    while (true) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientAddrLen = sizeof(clientAddr);
#else
        socklen_t clientAddrLen = sizeof(clientAddr);
#endif
        SocketType clientSock = accept(serverSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            logMessage("Failed to accept client connection.", LOG_ERROR);
            continue;
        }

        std::thread(handleClient, clientSock).detach();
    }

    stopMiningScheduler();

    if (serverConfig.enableRcon) {
        rconServer->stop();
    }

    if (queryServer) {
        queryServer->stop();
    }

#ifdef _WIN32
    closesocket(serverSock);
    WSACleanup();
#else
    close(serverSock);
#endif
}

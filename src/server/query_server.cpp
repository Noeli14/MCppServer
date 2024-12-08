#include "query_server.h"

#include "core/config.h"
#include "core/server.h"

QueryServer::QueryServer(const uint16_t port) : udpSocket(INVALID_SOCKET), port(port), running(false) {}

QueryServer::~QueryServer() {
    stop();
}

void QueryServer::start() {
    if (running.load()) return;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logMessage("[QueryServer] Failed to initialize Winsock for Query Protocol.", LOG_ERROR);
        return;
    }
#endif

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        logMessage("[QueryServer] Failed to create UDP socket for Query Protocol.", LOG_ERROR);
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(port);

    if (bind(udpSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        logMessage("[QueryServer] Failed to bind UDP socket for Query Protocol on port " + std::to_string(port), LOG_ERROR);
#ifdef _WIN32
        closesocket(udpSocket);
        WSACleanup();
#else
        close(udpSocket);
#endif
        return;
    }

    running.store(true);
    listenerThread = std::thread(&QueryServer::listenLoop, this);
    logMessage("[QueryServer] Query Protocol server started on UDP port " + std::to_string(port) + ".", LOG_INFO);
}

void QueryServer::stop() {
    if (!running.load()) return;

    running.store(false);
#ifdef _WIN32
    closesocket(udpSocket);
    WSACleanup();
#else
    close(udpSocket);
#endif
    if (listenerThread.joinable()) {
        listenerThread.join();
    }
}

void QueryServer::listenLoop() {
    while (running.load()) {
        uint8_t buffer[1024];
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        ssize_t received = recvfrom(udpSocket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (received == SOCKET_ERROR) {
            if (running.load()) {
                logMessage("[QueryServer] Error receiving UDP packet for Query Protocol.", LOG_ERROR);
            }
            continue;
        }

        std::string clientKey = sockaddrToString(clientAddr);
        handlePacket(buffer, received, clientAddr);
    }
}

void QueryServer::handlePacket(const uint8_t* data, size_t length, sockaddr_in clientAddr) {
    if (length < 6) { // Minimum packet size: magic(2) + type(1) + sessionId(4)
        logMessage("[QueryServer] Received malformed Query Protocol packet (too short) from " + sockaddrToString(clientAddr), LOG_ERROR);
        return;
    }

    // Check magic number
    if (data[0] != 0xFE || data[1] != 0xFD) {
        logMessage("[QueryServer] Invalid magic number in Query Protocol packet from " + sockaddrToString(clientAddr), LOG_ERROR);
        return;
    }

    uint8_t type = data[2];
    uint32_t sessionId = 0;
    // Correct sessionId extraction: bytes 3 to 6
    sessionId |= static_cast<uint32_t>(data[3]) << 24;
    sessionId |= static_cast<uint32_t>(data[4]) << 16;
    sessionId |= static_cast<uint32_t>(data[5]) << 8;
    sessionId |= static_cast<uint32_t>(data[6]);

    if (type == 0x09) { // Handshake
        // Generate and store challenge token
        int32_t challenge = generateChallengeToken();

        {
            std::lock_guard<std::mutex> lock(tokensMutex);
            std::string clientKey = sockaddrToString(clientAddr);
            challengeTokens[clientKey] = ChallengeToken{challenge, clientAddr, std::chrono::steady_clock::now()};
        }

        // Convert challenge to string and send response
        std::string challengeStr = std::to_string(challenge) + '\0';
        sendChallengeResponse(type, sessionId, challengeStr, clientAddr);
    }
    else if (type == 0x00) { // Stat request
        if (length < 10) { // FE FD 00 sessionId(4) challengeToken(4)
            logMessage("[QueryServer] Received malformed Stat request packet (too short) from " + sockaddrToString(clientAddr), LOG_ERROR);
            return;
        }

        int32_t challengeToken = 0;
        challengeToken |= static_cast<int32_t>(data[7]) << 24;
        challengeToken |= static_cast<int32_t>(data[8]) << 16;
        challengeToken |= static_cast<int32_t>(data[9]) << 8;
        challengeToken |= static_cast<int32_t>(data[10]);

        // Verify challenge token
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(tokensMutex);
            std::string clientKey = sockaddrToString(clientAddr);
            auto it = challengeTokens.find(clientKey);
            if (it != challengeTokens.end()) {
                if (it->second.token == challengeToken) {
                    valid = true;
                    challengeTokens.erase(it);
                }
            }
        }

        bool fullStat;
        if (length == 15) {
            fullStat = true;
        } else {
            fullStat = false;
        }

        if (valid) {
            sendStatResponse(sessionId, challengeToken, clientAddr, fullStat);
        }
        else {
            // Invalid token; do not respond
            logMessage("[QueryServer] Invalid challenge token from " + sockaddrToString(clientAddr), LOG_ERROR);
        }
    }
    else {
        logMessage("[QueryServer] Unknown Query Protocol packet type: " + std::to_string(type) + " from " + sockaddrToString(clientAddr), LOG_ERROR);
    }

    // Clean up expired tokens
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(tokensMutex);
    for (auto it = challengeTokens.begin(); it != challengeTokens.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() > 30) {
            it = challengeTokens.erase(it);
        }
        else {
            ++it;
        }
    }
}

void QueryServer::sendChallengeResponse(uint8_t type, uint32_t sessionId, const std::string& payload, sockaddr_in clientAddr) const {
    std::vector<uint8_t> response;

    // Type
    response.push_back(type);
    // Session ID
    response.push_back((sessionId >> 24) & 0xFF);
    response.push_back((sessionId >> 16) & 0xFF);
    response.push_back((sessionId >> 8) & 0xFF);
    response.push_back(sessionId & 0xFF);
    // Payload (challenge token string)
    response.insert(response.end(), payload.begin(), payload.end());

    ssize_t sent = sendto(udpSocket, reinterpret_cast<char*>(response.data()), response.size(), 0,
                         reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));
    if (sent == SOCKET_ERROR) {
        logMessage("[QueryServer] Failed to send challenge response to " + sockaddrToString(clientAddr), LOG_ERROR);
    }
}

void QueryServer::sendStatResponse(uint32_t sessionId, int32_t challengeToken, sockaddr_in clientAddr, bool fullStat) const {
    std::vector<uint8_t> response;

    // Type
    response.push_back(0x00);
    // Session ID
    response.push_back((sessionId >> 24) & 0xFF);
    response.push_back((sessionId >> 16) & 0xFF);
    response.push_back((sessionId >> 8) & 0xFF);
    response.push_back(sessionId & 0xFF);

    std::string hostIP = "127.0.0.1"; // For now

    if (fullStat) {
        // 11 bytes of padding
        for (int i = 0; i < 11; i++) {
            response.push_back(0x00);
        }

        // Payload construction: K,V pairs
        std::string payload;

        std::string plugins = "";

        payload += "hostname";
        payload.append(1, '\0');
        payload += serverConfig.motd;
        payload.append(1, '\0');
        payload += "gametype";
        payload.append(1, '\0');
        payload += "SMP";
        payload.append(1, '\0');
        payload += "game_id";
        payload.append(1, '\0');
        payload += "Minecraft";
        payload.append(1, '\0');
        payload += "version";
        payload.append(1, '\0');
        payload += serverConfig.server_version;
        payload.append(1, '\0');
        payload += "plugins";
        payload.append(1, '\0');
        payload += plugins;
        payload.append(1, '\0');
        payload += "map";
        payload.append(1, '\0');
        payload += serverConfig.worldType;
        payload.append(1, '\0');
        payload += "numplayers";
        payload.append(1, '\0');
        payload += std::to_string(globalPlayers.size());
        payload.append(1, '\0');
        payload += "maxplayers";
        payload.append(1, '\0');
        payload += std::to_string(serverConfig.maxPlayers);
        payload.append(1, '\0');
        payload += "hostport";
        payload.append(1, '\0');
        payload += std::to_string(serverConfig.port);
        payload.append(1, '\0');
        payload += "hostip";
        payload.append(1, '\0');
        payload += hostIP;
        payload.append(1, '\0');

        // Terminate K,V section with a double null
        payload.append(1, '\0');

        // Append K,V payload to response
        response.insert(response.end(), payload.begin(), payload.end());

        // 10 bytes of padding
        for (int i = 0; i < 10; i++) {
            response.push_back(0x00);
        }

        // Players section
        std::string players = "";

        if (globalPlayers.empty()) {
            players = '\0';
        } else {
            for (const auto &key: globalPlayersName | std::views::keys) {
                players += key;
                players.append(1, '\0');
            }
            players.append(1, '\0');
        }

        players.append(1, '\0');

        response.insert(response.end(), players.begin(), players.end());
    } else {
        std::string payload;
        payload += serverConfig.motd;
        payload.append(1, '\0');
        payload += "SMP";
        payload.append(1, '\0');
        payload += serverConfig.worldType;
        payload.append(1, '\0');
        payload += std::to_string(globalPlayers.size());
        payload.append(1, '\0');
        payload += std::to_string(serverConfig.maxPlayers);
        payload.append(1, '\0');

        response.insert(response.end(), payload.begin(), payload.end());
        uint16_t hostPort = serverConfig.port;
        response.push_back(hostPort & 0xFF);
        response.push_back((hostPort >> 8) & 0xFF);

        payload = hostIP;
        payload.append(1, '\0');

        response.insert(response.end(), payload.begin(), payload.end());
    }

    // Send the response
    ssize_t sent = sendto(udpSocket, reinterpret_cast<char*>(response.data()), response.size(), 0,
                         reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));
    if (sent == SOCKET_ERROR) {
        logMessage("[QueryServer] Failed to send stat response to " + sockaddrToString(clientAddr), LOG_ERROR);
    }
}

#include "rcon_server.h"

#include "networking//client.h"
#include "commands/CommandBuilder.h"
#include "utils/le32toh.h"
#include "core/utils.h"
#include "utils/translation.h"
#include "utils/thread_pool.h"


RCONServer::RCONServer(int port, const std::string& password)
    : port(port), password(password), running(false) {}

RCONServer::~RCONServer() {
    stop();
}

bool RCONServer::start() {
    running = true;
    listenThread = std::thread(&RCONServer::listenLoop, this);
	set_thread_name(listenThread, "RconServer");
    logMessage("RCON server started on port " + std::to_string(port), LOG_INFO);
    return true;
}

void RCONServer::stop() {
    if (running) {
        running = false;
        if (listenThread.joinable()) {
            listenThread.join();
        }

        // Join all client threads
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& t : clientThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        clientThreads.clear();
        logMessage("RCON server stopped.", LOG_INFO);
    }
}

void RCONServer::listenLoop() {
    // Initialize socket
#ifdef _WIN32
    WSADATA wsaData;
    int wsaStartup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaStartup != 0) {
        logMessage("RCON: Failed to initialize Winsock.", LOG_ERROR);
        return;
    }
#endif

    SocketType listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        logMessage("RCON: Failed to create socket.", LOG_ERROR);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // Set socket options
    int opt = 1;
#ifdef _WIN32
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        logMessage("RCON: Failed to bind socket.", LOG_ERROR);
#ifdef _WIN32
        closesocket(listenSock);
        WSACleanup();
#else
        close(listenSock);
#endif
        return;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        logMessage("RCON: Failed to listen on socket.", LOG_ERROR);
#ifdef _WIN32
        closesocket(listenSock);
        WSACleanup();
#else
        close(listenSock);
#endif
        return;
    }

    while (running) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientAddrLen = sizeof(clientAddr);
#else
        socklen_t clientAddrLen = sizeof(clientAddr);
#endif
        SocketType clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            if (running) {
                logMessage("RCON: Failed to accept client connection.", LOG_ERROR);
            }
            continue;
        }

        // Handle the client in a separate thread
        std::lock_guard lock(clientsMutex);
        clientThreads.emplace_back(&RCONServer::handleClient, this, clientSock);
    }

    // Clean up
#ifdef _WIN32
    closesocket(listenSock);
    WSACleanup();
#else
    close(listenSock);
#endif
}

void RCONServer::handleClient(SocketType clientSock) const {
    if (!authenticateClient(clientSock)) {
        logMessage("RCON: Client authentication failed.", LOG_WARNING);
#ifdef _WIN32
        closesocket(clientSock);
#else
        close(clientSock);
#endif
        return;
    }

    logMessage("RCON: Client authenticated successfully.", LOG_INFO);

    while (running) {
        // Read the packet length first (4 bytes, little-endian)
        uint32_t packetLength = 0;
        ssize_t bytesRead = recv(clientSock, reinterpret_cast<char*>(&packetLength), sizeof(packetLength), MSG_WAITALL);
        if (bytesRead <= 0) {
            break; // Client disconnected or error
        }

        // Convert from little-endian to host byte order
        packetLength = le32toh(packetLength);

        if (packetLength < 10) { // Minimum packet size
            logMessage("RCON: Received invalid packet length.", LOG_WARNING);
            break;
        }

        // Read the rest of the packet
        std::vector<char> buffer(packetLength);
        bytesRead = recv(clientSock, buffer.data(), packetLength, MSG_WAITALL);
        if (bytesRead <= 0) {
            break;
        }

        // Parse the packet
        int32_t requestId = *reinterpret_cast<int32_t*>(buffer.data());
        int32_t type = *reinterpret_cast<int32_t*>(buffer.data() + 4);
        char* payload = buffer.data() + 8;
        std::string payloadStr(payload);

        if (type == 3) { // Login
            // Already authenticated earlier, send success response
            // In this implementation, authentication is done before handling packets
            // So we can ignore additional login packets or handle re-authentication if needed
            // For simplicity, we do nothing
        } else if (type == 2) { // Command
            processCommand(clientSock, payloadStr, requestId);
        } else {
            // Unknown packet type
            // Send error response
            uint32_t responseLength = 10 + 19; // length + requestId + type + payload + null
            std::vector<char> response(responseLength, 0);
            int32_t responseId = -1;
            int32_t responseType = 0; // Response packet
            memcpy(response.data(), &responseLength, 4);
            memcpy(response.data() + 4, &responseId, 4);
            memcpy(response.data() + 8, &responseType, 4);
            std::string errorMsg = "Unknown request type " + std::to_string(type);
            memcpy(response.data() + 12, errorMsg.c_str(), errorMsg.size());
            send(clientSock, response.data(), response.size(), 0);
        }
    }

    logMessage("RCON: Client disconnected.", LOG_INFO);
#ifdef _WIN32
    closesocket(clientSock);
#else
    close(clientSock);
#endif
}

bool RCONServer::authenticateClient(SocketType clientSock) const {
    // In the provided protocol, authentication is done via a login packet (type 3)
    // Here, we'll expect the first packet to be a login packet

    // Read the packet length first (4 bytes, little-endian)
    uint32_t packetLength = 0;
    ssize_t bytesRead = recv(clientSock, reinterpret_cast<char*>(&packetLength), sizeof(packetLength), MSG_WAITALL);
    if (bytesRead <= 0) {
        return false;
    }

    // Convert from little-endian to host byte order
    packetLength = le32toh(packetLength);

    if (packetLength < 10) { // Minimum packet size
        logMessage("RCON: Received invalid authentication packet length.", LOG_WARNING);
        return false;
    }

    // Read the rest of the packet
    std::vector<char> buffer(packetLength);
    bytesRead = recv(clientSock, buffer.data(), packetLength, MSG_WAITALL);
    if (bytesRead <= 0) {
        return false;
    }

    // Parse the packet
    int32_t requestId = *reinterpret_cast<int32_t*>(buffer.data());
    int32_t type = *reinterpret_cast<int32_t*>(buffer.data() + 4);
    char* payload = buffer.data() + 8;
    std::string passwordAttempt(payload);

    if (type != 3) { // Expecting login packet
        logMessage("RCON: First packet is not a login packet.", LOG_WARNING);
        return false;
    }

    if (passwordAttempt != password) {
        // Send auth failure response
        uint32_t responseLength = 10;
        std::vector<char> response(4 + responseLength, 0);
        int32_t responseId = -1;
        int32_t responseType = 0; // Type 0 for response
        memcpy(response.data(), &responseLength, 4);
        memcpy(response.data() + 4, &responseId, 4);
        memcpy(response.data() + 8, &responseType, 4);
        send(clientSock, response.data(), response.size(), 0);
        return false;
    }

    // Send auth success response
    uint32_t successLength = 10;
    std::vector<char> successResponse(14, 0);
    int32_t successId = requestId;
    int32_t successType = 0; // Type 0 for response
    memcpy(successResponse.data(), &successLength, 4);
    memcpy(successResponse.data() + 4, &successId, 4);
    memcpy(successResponse.data() + 8, &successType, 4);
    send(clientSock, successResponse.data(), successResponse.size(), 0);

    return true;
}

void RCONServer::processCommand(SocketType clientSock, const std::string& command, int requestId) {
    // Execute the command on the server and capture the output
    logMessage("Received command: " + command, LOG_INFO);
    std::string commandOutput;

    auto outputCollector = [&](const std::string& message, bool isError, const std::vector<std::string>& args) {
        std::string formattedMessage = removeFormattingCodes(message);
        std::vector<std::string> formattedArgs;
        for (const auto& arg : args) {
            formattedArgs.push_back(removeFormattingCodes(arg));
        }
        commandOutput += getTranslationString(formattedMessage, consoleLang, formattedArgs) + "\n"; // Append messages with newline
    };

    // Create a CommandParser instance with the command graph and output collector
    CommandParser parser(globalCommandGraph, outputCollector);

    // Execute the command
    bool success = parser.parseAndExecuteConsole(command, outputCollector);

    // Handle fragmentation if output is larger than 4096 bytes
    size_t totalLength = commandOutput.size();
    size_t offset = 0;

    while (offset < totalLength) {
        size_t maxPayload = 4096;
        size_t chunkSize = std::min(maxPayload, totalLength - offset);
        std::string chunk = commandOutput.substr(offset, chunkSize);
        offset += chunkSize;

        // Prepare the response packet
        uint32_t payloadLength = 10 + chunk.size(); // 4 (Request ID) + 4 (Type) + payload size + 2 (Null Bytes)
        std::vector<char> response(4 + payloadLength, 0); // 4 bytes for Length + payload
        int32_t responseId = requestId;
        int32_t responseType = 2; // Command response

        memcpy(response.data(), &payloadLength, 4);
        memcpy(response.data() + 4, &responseId, 4);
        memcpy(response.data() + 8, &responseType, 4);
        memcpy(response.data() + 12, chunk.c_str(), chunk.size());
        // Ensure two null bytes at the end (already set to 0 by initializing the vector)
        response[12 + chunk.size()] = '\x00'; // First null terminator
        response[12 + chunk.size() + 1] = '\x00'; // Second null terminator

        // Send the response packet
        send(clientSock, response.data(), response.size(), 0);
    }
}

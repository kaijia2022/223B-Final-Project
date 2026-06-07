#include "BottomLayer.h"
#include <iostream>
#include <algorithm>
#include <utility>

// Include Winsock
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")

static bool RecvExact(SOCKET socketHandle, char* dst, int byteCount) {
    int totalReceived = 0;
    while (totalReceived < byteCount) {
        int received = recv(socketHandle, dst + totalReceived, byteCount - totalReceived, 0);
        if (received <= 0) {
            return false;
        }
        totalReceived += received;
    }
    return true;
}

static bool SendAll(SOCKET socketHandle, const char* src, int byteCount) {
    int totalSent = 0;
    while (totalSent < byteCount) {
        int sent = send(socketHandle, src + totalSent, byteCount - totalSent, 0);
        if (sent <= 0) {
            return false;
        }
        totalSent += sent;
    }
    return true;
}

static int PacketSizeFromType(PacketType type) {
    switch (type) {
    case PacketType::CLIENT_INPUT: return sizeof(ClientInputPacket);
    case PacketType::GAME_STATE: return sizeof(GameStatePacket);
    case PacketType::PLAYER_ASSIGNMENT: return sizeof(PlayerAssignmentPacket);
    default: return 0;
    }
}

BottomLayer::BottomLayer()
    : isRunning(false), isHost(false), activeSocket(0), listenSocket(0), localPlayerId(0),
      outboundDelayRng(std::random_device{}()), outboundDelayEnabled(false),
      outboundDelayMinMs(0), outboundDelayMaxMs(0), outboundSenderStop(false) {
    playerSockets.fill(0);
    connectedPlayers.fill(false);
    connectedPlayers[0] = true; // Host/local slot exists by default when hosting.

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Dedicated delayed-send worker. This keeps artificial outbound delay
    // independent of the render/game loop.
    outboundSenderThread = std::thread(&BottomLayer::OutboundSenderLoop, this);
}

BottomLayer::~BottomLayer() {
    isRunning = false;
    outboundSenderStop = true;
    outboundDelayCv.notify_all();
    if (outboundSenderThread.joinable()) {
        outboundSenderThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex);
        if (listenSocket != 0) {
            closesocket((SOCKET)listenSocket);
            listenSocket = 0;
        }
        if (activeSocket != 0) {
            closesocket((SOCKET)activeSocket);
            activeSocket = 0;
        }
        for (uintptr_t& sock : playerSockets) {
            if (sock != 0) {
                closesocket((SOCKET)sock);
                sock = 0;
            }
        }
    }

    if (networkThread.joinable()) {
        networkThread.join();
    }
    for (std::thread& t : clientThreads) {
        if (t.joinable()) {
            t.join();
        }
    }

    WSACleanup();
}

bool BottomLayer::HostGame(int port) {
    isHost = true;
    isRunning = true;
    localPlayerId = 0;
    connectedPlayers.fill(false);
    connectedPlayers[0] = true;

    SOCKET newListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (newListenSocket == INVALID_SOCKET) {
        std::cout << "Failed to create listen socket.\n";
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Listen on all adapters (Localhost & Hamachi)
    serverAddr.sin_port = htons(port);

    if (bind(newListenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Bind failed.\n";
        closesocket(newListenSocket);
        return false;
    }

    // Up to 3 remote clients + the host = 4 total players.
    if (listen(newListenSocket, MAX_PLAYERS - 1) == SOCKET_ERROR) {
        std::cout << "Listen failed.\n";
        closesocket(newListenSocket);
        return false;
    }

    listenSocket = (uintptr_t)newListenSocket;
    std::cout << "Hosting server on port " << port << "... Waiting for up to " << (MAX_PLAYERS - 1) << " clients.\n";

    // Start background thread to accept multiple clients.
    networkThread = std::thread(&BottomLayer::HostAcceptLoop, this, listenSocket);
    return true;
}

bool BottomLayer::ConnectToGame(const std::string& virtualIp, int port) {
    isHost = false;
    isRunning = true;
    localPlayerId = 1; // Temporary fallback until the host assignment arrives.

    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        std::cout << "Failed to create client socket.\n";
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, virtualIp.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(port);

    std::cout << "Connecting to " << virtualIp << ":" << port << "...\n";

    if (connect(connectSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Connection failed.\n";
        closesocket(connectSocket);
        return false;
    }

    // The host immediately sends a PlayerAssignmentPacket so this client knows whether it is P1, P2, or P3.
    PlayerAssignmentPacket assignment{};
    bool receivedAssignment = RecvExact(connectSocket, reinterpret_cast<char*>(&assignment), sizeof(assignment));
    if (!receivedAssignment || assignment.type != PacketType::PLAYER_ASSIGNMENT || assignment.playerId >= MAX_PLAYERS) {
        std::cout << "Failed to receive a valid player assignment from host.\n";
        closesocket(connectSocket);
        return false;
    }

    localPlayerId = assignment.playerId;
    std::cout << "Connected to Host as Player " << localPlayerId << "!\n";
    this->activeSocket = (uintptr_t)connectSocket;

    connectedPlayers.fill(false);
    connectedPlayers[0] = true;
    connectedPlayers[localPlayerId] = true;

    // Start background thread to listen to the server.
    networkThread = std::thread(&BottomLayer::NetworkWorkerLoop, this, activeSocket, localPlayerId);
    return true;
}

uint32_t BottomLayer::GetLocalPlayerId() const {
    return localPlayerId;
}

bool BottomLayer::IsPlayerConnected(uint32_t playerId) const {
    if (playerId >= MAX_PLAYERS) return false;
    std::lock_guard<std::mutex> lock(socketMutex);
    return connectedPlayers[playerId];
}

uint32_t BottomLayer::ReserveClientSlot() {
    std::lock_guard<std::mutex> lock(socketMutex);
    for (uint32_t i = 1; i < MAX_PLAYERS; ++i) {
        if (!connectedPlayers[i]) {
            connectedPlayers[i] = true;
            return i;
        }
    }
    return MAX_PLAYERS;
}

void BottomLayer::HostAcceptLoop(uintptr_t listenSocketHandle) {
    while (isRunning) {
        SOCKET clientSocket = accept((SOCKET)listenSocketHandle, NULL, NULL);
        if (!isRunning) break;

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                std::cout << "Accept failed or listener closed.\n";
            }
            break;
        }

        uint32_t assignedPlayerId = ReserveClientSlot();
        if (assignedPlayerId >= MAX_PLAYERS) {
            std::cout << "Rejected client: lobby is full.\n";
            closesocket(clientSocket);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(socketMutex);
            playerSockets[assignedPlayerId] = (uintptr_t)clientSocket;
        }

        PlayerAssignmentPacket assignment{};
        assignment.playerId = assignedPlayerId;
        if (!SendAll(clientSocket, reinterpret_cast<const char*>(&assignment), sizeof(assignment))) {
            std::cout << "Failed to send player assignment to client.\n";
            MarkPlayerDisconnected(assignedPlayerId, (uintptr_t)clientSocket);
            continue;
        }

        std::cout << "Client connected as Player " << assignedPlayerId << "!\n";
        clientThreads.emplace_back(&BottomLayer::NetworkWorkerLoop, this, (uintptr_t)clientSocket, assignedPlayerId);
    }
}

void BottomLayer::SendNetworkData(const std::string& payload) {
    if (payload.empty()) return;

    if (isHost) {
        std::vector<std::pair<uint32_t, uintptr_t>> socketsToSend;
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            for (uint32_t i = 1; i < MAX_PLAYERS; ++i) {
                uintptr_t sock = playerSockets[i];
                if (connectedPlayers[i] && sock != 0) {
                    socketsToSend.push_back({ i, sock });
                }
            }
        }

        for (const auto& entry : socketsToSend) {
            if (outboundDelayEnabled) {
                QueueDelayedOutbound(entry.first, entry.second, payload);
            }
            else {
                SendPayloadImmediate(entry.first, entry.second, payload);
            }
        }
    }
    else if (activeSocket != 0) {
        uintptr_t sock = activeSocket;
        if (outboundDelayEnabled) {
            QueueDelayedOutbound(0, sock, payload);
        }
        else {
            SendPayloadImmediate(localPlayerId, sock, payload);
        }
    }
}

void BottomLayer::SetOutboundDelayRange(int minDelayMs, int maxDelayMs) {
    if (minDelayMs < 0) minDelayMs = 0;
    if (maxDelayMs < 0) maxDelayMs = 0;
    if (minDelayMs > maxDelayMs) std::swap(minDelayMs, maxDelayMs);

    outboundDelayMinMs = minDelayMs;
    outboundDelayMaxMs = maxDelayMs;
    outboundDelayEnabled = (maxDelayMs > 0);
}

void BottomLayer::ClearOutboundDelay() {
    outboundDelayEnabled = false;
    outboundDelayMinMs = 0;
    outboundDelayMaxMs = 0;

    {
        std::lock_guard<std::mutex> lock(outboundDelayMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto& packet : outboundDelayQueue) {
            packet.sendTime = now;
        }
    }

    outboundDelayCv.notify_all();
}

bool BottomLayer::IsOutboundDelayEnabled() const {
    return outboundDelayEnabled;
}

int BottomLayer::GetOutboundDelayMinMs() const {
    return outboundDelayMinMs;
}

int BottomLayer::GetOutboundDelayMaxMs() const {
    return outboundDelayMaxMs;
}

void BottomLayer::FlushDelayedOutboundPackets() {
    // The dedicated sender thread performs timed flushing. This only wakes it
    // so packets that are already due can be released promptly.
    outboundDelayCv.notify_one();
}

int BottomLayer::ComputeOutboundDelayMs() {
    const int minDelay = outboundDelayMinMs.load();
    const int maxDelay = outboundDelayMaxMs.load();
    if (maxDelay <= 0) return 0;
    if (minDelay >= maxDelay) return maxDelay;

    std::uniform_int_distribution<int> distribution(minDelay, maxDelay);
    return distribution(outboundDelayRng);
}

void BottomLayer::QueueDelayedOutbound(uint32_t targetPlayerId, uintptr_t socketHandle, const std::string& payload) {
    if (payload.empty() || socketHandle == 0) return;

    DelayedOutboundPacket packet;
    packet.targetPlayerId = targetPlayerId;
    packet.socketHandle = socketHandle;
    packet.payload = payload;

    {
        std::lock_guard<std::mutex> lock(outboundDelayMutex);
        packet.sendTime = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(ComputeOutboundDelayMs());
        outboundDelayQueue.push_back(std::move(packet));
    }

    outboundDelayCv.notify_one();
}

void BottomLayer::OutboundSenderLoop() {
    while (!outboundSenderStop.load()) {
        std::deque<DelayedOutboundPacket> readyPackets;

        {
            std::unique_lock<std::mutex> lock(outboundDelayMutex);

            if (outboundDelayQueue.empty()) {
                outboundDelayCv.wait(lock, [this] {
                    return outboundSenderStop.load() || !outboundDelayQueue.empty();
                });
            }
            else {
                auto nextIt = std::min_element(
                    outboundDelayQueue.begin(),
                    outboundDelayQueue.end(),
                    [](const DelayedOutboundPacket& a, const DelayedOutboundPacket& b) {
                        return a.sendTime < b.sendTime;
                    }
                );

                if (nextIt != outboundDelayQueue.end()) {
                    outboundDelayCv.wait_until(lock, nextIt->sendTime, [this] {
                        return outboundSenderStop.load();
                    });
                }
            }

            if (outboundSenderStop.load()) break;

            const auto now = std::chrono::steady_clock::now();
            auto it = outboundDelayQueue.begin();
            while (it != outboundDelayQueue.end()) {
                if (it->sendTime <= now) {
                    readyPackets.push_back(std::move(*it));
                    it = outboundDelayQueue.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        for (const DelayedOutboundPacket& packet : readyPackets) {
            SendPayloadImmediate(packet.targetPlayerId, packet.socketHandle, packet.payload);
        }
    }
}

bool BottomLayer::SendPayloadImmediate(uint32_t targetPlayerId, uintptr_t socketHandle, const std::string& payload) {
    if (socketHandle == 0 || payload.empty()) return false;

    if (!SendAll((SOCKET)socketHandle, payload.data(), (int)payload.size())) {
        if (isHost) {
            std::cout << "Failed to send data to Player " << targetPlayerId << ".\n";
            MarkPlayerDisconnected(targetPlayerId, socketHandle);
        }
        else {
            std::cout << "Failed to send data to host.\n";
            MarkPlayerDisconnected(localPlayerId, socketHandle);
        }
        return false;
    }

    return true;
}

bool BottomLayer::HasIncomingData() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return !incomingDataQueue.empty();
}

std::string BottomLayer::GetNextNetworkMessage() {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (incomingDataQueue.empty()) return "";

    std::string msg = incomingDataQueue.front();
    incomingDataQueue.pop();
    return msg;
}

void BottomLayer::NetworkWorkerLoop(uintptr_t socketHandle, uint32_t playerId) {
    while (isRunning && socketHandle != 0) {
        PacketType type{};
        if (!RecvExact((SOCKET)socketHandle, reinterpret_cast<char*>(&type), sizeof(type))) {
            std::cout << "Player " << playerId << " disconnected.\n";
            MarkPlayerDisconnected(playerId, socketHandle);
            break;
        }

        int packetSize = PacketSizeFromType(type);
        if (packetSize <= 0) {
            std::cout << "Received invalid packet type from Player " << playerId << ".\n";
            MarkPlayerDisconnected(playerId, socketHandle);
            break;
        }

        std::string rawData(packetSize, '\0');
        rawData[0] = static_cast<char>(type);

        if (!RecvExact((SOCKET)socketHandle, rawData.data() + sizeof(type), packetSize - (int)sizeof(type))) {
            std::cout << "Player " << playerId << " disconnected during packet receive.\n";
            MarkPlayerDisconnected(playerId, socketHandle);
            break;
        }

        std::lock_guard<std::mutex> lock(queueMutex);
        incomingDataQueue.push(rawData);
    }
}

void BottomLayer::MarkPlayerDisconnected(uint32_t playerId, uintptr_t socketHandle) {
    std::lock_guard<std::mutex> lock(socketMutex);

    if (playerId < MAX_PLAYERS) {
        connectedPlayers[playerId] = false;
        if (playerSockets[playerId] == socketHandle) {
            playerSockets[playerId] = 0;
        }
    }

    if (activeSocket == socketHandle) {
        activeSocket = 0;
        isRunning = false;
    }

    if (socketHandle != 0) {
        closesocket((SOCKET)socketHandle);
    }
}

void BottomLayer::InjectKeyDown(int keycode) {
    injectedKeyStates[keycode] = true;
}

void BottomLayer::InjectKeyUp(int keycode) {
    injectedKeyStates[keycode] = false;
}

bool BottomLayer::IsActionPressed(int keycode) {
    auto it = injectedKeyStates.find(keycode);
    return it != injectedKeyStates.end() && it->second;
}

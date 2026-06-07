#include "BottomLayer.h"
#include "Engine.h"
#include <algorithm>
#include <iostream>
#include <utility>

// Include Winsock
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")

BottomLayer::BottomLayer()
    : isRunning(false), isHost(false), activeSocket(0), listenSocketHandle(0),
      outboundDelayRng(std::random_device{}()), outboundDelayEnabled(false),
      outboundDelayMinMs(0), outboundDelayMaxMs(0), outboundSenderStop(false) {
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

    if (listenSocketHandle != 0) {
        closesocket((SOCKET)listenSocketHandle);
        listenSocketHandle = 0;
    }

    if (activeSocket != 0) {
        closesocket((SOCKET)activeSocket);
        activeSocket = 0;
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex);
        for (uintptr_t socketHandle : clientSockets) {
            if (socketHandle != 0) {
                closesocket((SOCKET)socketHandle);
            }
        }
        clientSockets.clear();
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

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    listenSocketHandle = (uintptr_t)listenSocket;

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Listen on all adapters (Localhost & Hamachi)
    serverAddr.sin_port = htons(port);

    bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
    listen(listenSocket, MAX_PLAYERS - 1); // Host is P0; allow P1-P3 clients.

    std::cout << "Hosting server on port " << port << "... Waiting for up to " << (MAX_PLAYERS - 1) << " clients.\n";

    // Start background thread to accept multiple clients.
    networkThread = std::thread([this, listenSocket]() {
        uint32_t nextPlayerId = 1;

        while (this->isRunning && nextPlayerId < MAX_PLAYERS) {
            SOCKET clientSocket = accept(listenSocket, NULL, NULL);
            if (clientSocket == INVALID_SOCKET) {
                if (this->isRunning) {
                    std::cout << "Accept failed or listener closed.\n";
                }
                break;
            }

            uint32_t assignedPlayerId = nextPlayerId++;
            {
                std::lock_guard<std::mutex> lock(this->socketMutex);
                this->clientSockets.push_back((uintptr_t)clientSocket);
            }

            AssignPlayerIdPacket assignPacket{ PacketType::ASSIGN_PLAYER_ID, assignedPlayerId };
            send(clientSocket, reinterpret_cast<const char*>(&assignPacket), sizeof(assignPacket), 0);

            {
                std::string joinEvent(reinterpret_cast<const char*>(&assignPacket), sizeof(assignPacket));
                std::lock_guard<std::mutex> lock(this->queueMutex);
                this->incomingDataQueue.push(joinEvent);
            }

            std::cout << "Client connected as player " << assignedPlayerId << "!\n";
            this->clientThreads.emplace_back(&BottomLayer::ClientReceiveLoop, this, (uintptr_t)clientSocket, assignedPlayerId);
        }

        closesocket(listenSocket);
        this->listenSocketHandle = 0;
        });

    return true;
}

bool BottomLayer::ConnectToGame(const std::string& virtualIp, int port) {
    isHost = false;
    isRunning = true;

    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, virtualIp.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(port);

    std::cout << "Connecting to " << virtualIp << ":" << port << "...\n";

    if (connect(connectSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Connection failed.\n";
        closesocket(connectSocket);
        return false;
    }

    std::cout << "Connected to Host! Waiting for player assignment...\n";
    this->activeSocket = connectSocket;

    // Start background thread to listen to the server
    networkThread = std::thread(&BottomLayer::NetworkWorkerLoop, this);
    return true;
}

void BottomLayer::SendNetworkData(const std::string& payload) {
    if (isHost) {
        std::lock_guard<std::mutex> lock(socketMutex);
        for (uintptr_t socketHandle : clientSockets) {
            if (socketHandle != 0) {
                if (outboundDelayEnabled) {
                    QueueDelayedOutbound(socketHandle, payload);
                }
                else {
                    SendImmediate(socketHandle, payload);
                }
            }
        }
    }
    else if (activeSocket != 0) {
        if (outboundDelayEnabled) {
            QueueDelayedOutbound(activeSocket, payload);
        }
        else {
            // Send the raw binary data over TCP
            SendImmediate(activeSocket, payload);
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

    // Do not drop already queued gameplay packets when disabling delay.
    // Send them now so F1 only removes artificial latency.
    std::deque<DelayedOutboundPacket> queuedPackets;
    {
        std::lock_guard<std::mutex> lock(outboundDelayMutex);
        queuedPackets.swap(outboundDelayQueue);
    }

    for (const DelayedOutboundPacket& packet : queuedPackets) {
        SendImmediate(packet.socketHandle, packet.payload);
    }

    outboundDelayCv.notify_all();
}

bool BottomLayer::IsOutboundDelayEnabled() const { return outboundDelayEnabled; }
int BottomLayer::GetOutboundDelayMinMs() const { return outboundDelayMinMs; }
int BottomLayer::GetOutboundDelayMaxMs() const { return outboundDelayMaxMs; }

int BottomLayer::ComputeOutboundDelayMs() {
    const int minDelay = outboundDelayMinMs.load();
    const int maxDelay = outboundDelayMaxMs.load();
    if (maxDelay <= 0) return 0;
    if (minDelay >= maxDelay) return maxDelay;

    std::uniform_int_distribution<int> distribution(minDelay, maxDelay);
    return distribution(outboundDelayRng);
}

void BottomLayer::QueueDelayedOutbound(uintptr_t socketHandle, const std::string& payload) {
    if (socketHandle == 0) return;

    DelayedOutboundPacket packet;
    packet.socketHandle = socketHandle;
    packet.payload = payload;
    packet.sendTime = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(ComputeOutboundDelayMs());

    {
        std::lock_guard<std::mutex> lock(outboundDelayMutex);
        outboundDelayQueue.push_back(std::move(packet));
    }
    outboundDelayCv.notify_one();
}

void BottomLayer::FlushDelayedOutboundPackets() {
    // The background sender owns scheduled sends. This method exists so the
    // game loop can keep the same interface as the rollback delay-injection build.
    outboundDelayCv.notify_one();
}

void BottomLayer::OutboundSenderLoop() {
    while (!outboundSenderStop) {
        std::deque<DelayedOutboundPacket> readyPackets;

        {
            std::unique_lock<std::mutex> lock(outboundDelayMutex);

            if (outboundDelayQueue.empty()) {
                outboundDelayCv.wait(lock, [this] {
                    return outboundSenderStop.load() || !outboundDelayQueue.empty();
                });
            }

            if (outboundSenderStop) break;

            auto nextIt = std::min_element(
                outboundDelayQueue.begin(),
                outboundDelayQueue.end(),
                [](const DelayedOutboundPacket& a, const DelayedOutboundPacket& b) {
                    return a.sendTime < b.sendTime;
                });

            if (nextIt != outboundDelayQueue.end()) {
                outboundDelayCv.wait_until(lock, nextIt->sendTime, [this] {
                    return outboundSenderStop.load();
                });
            }

            if (outboundSenderStop) break;

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
            SendImmediate(packet.socketHandle, packet.payload);
        }
    }
}

void BottomLayer::SendImmediate(uintptr_t socketHandle, const std::string& payload) {
    if (socketHandle == 0 || payload.empty()) return;
    send((SOCKET)socketHandle, payload.data(), (int)payload.size(), 0);
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

void BottomLayer::ClientReceiveLoop(uintptr_t socketHandle, uint32_t assignedPlayerId) {
    char buffer[4096];
    SOCKET clientSocket = (SOCKET)socketHandle;

    while (isRunning && socketHandle != 0) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);

        if (bytesReceived > 0) {
            // Package the raw bytes into a string safely. The game layer still
            // reads the same packet format; we only make sure each socket is
            // mapped to its assigned player slot on the host.
            std::string rawData(buffer, bytesReceived);

            size_t offset = 0;
            while (offset + sizeof(ClientInputPacket) <= rawData.size()) {
                PacketType type = static_cast<PacketType>(rawData[offset]);
                if (type != PacketType::CLIENT_INPUT) break;

                ClientInputPacket* input = reinterpret_cast<ClientInputPacket*>(&rawData[offset]);
                input->playerId = assignedPlayerId;
                offset += sizeof(ClientInputPacket);
            }

            std::lock_guard<std::mutex> lock(queueMutex);
            incomingDataQueue.push(rawData);
        }
        else if (bytesReceived == 0 || bytesReceived == SOCKET_ERROR) {
            std::cout << "Player " << assignedPlayerId << " disconnected.\n";
            break;
        }
    }
}

void BottomLayer::NetworkWorkerLoop() {
    char buffer[4096];
    while (isRunning && activeSocket != 0) {
        // Block and wait for data
        int bytesReceived = recv((SOCKET)activeSocket, buffer, sizeof(buffer), 0);

        if (bytesReceived > 0) {
            // Package the raw bytes into a string safely
            std::string rawData(buffer, bytesReceived);

            std::lock_guard<std::mutex> lock(queueMutex);
            incomingDataQueue.push(rawData);
        }
        else if (bytesReceived == 0 || bytesReceived == SOCKET_ERROR) {
            std::cout << "Connection closed by peer.\n";
            isRunning = false;
            break;
        }
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

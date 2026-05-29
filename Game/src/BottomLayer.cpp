#include "BottomLayer.h"
#include "Engine.h"
#include <iostream>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

namespace {
    constexpr int MAX_REMOTE_CLIENTS = MAX_PLAYERS - 1;

    bool RecvExact(SOCKET socketHandle, char* dst, int byteCount) {
        int receivedTotal = 0;
        while (receivedTotal < byteCount) {
            int received = recv(socketHandle, dst + receivedTotal, byteCount - receivedTotal, 0);
            if (received <= 0) return false;
            receivedTotal += received;
        }
        return true;
    }
}

BottomLayer::BottomLayer() : isHost(false), isRunning(false) {
    WSADATA wsaData;
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cout << "WSAStartup failed: " << result << "\n";
    }
}

BottomLayer::~BottomLayer() {
    isRunning = false;

    if (listenSocket != 0) {
        closesocket((SOCKET)listenSocket);
        listenSocket = 0;
    }
    if (activeSocket != 0) {
        closesocket((SOCKET)activeSocket);
        activeSocket = 0;
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex);
        for (uintptr_t s : clientSockets) {
            if (s != 0) closesocket((SOCKET)s);
        }
        clientSockets.clear();
    }

    if (acceptThread.joinable()) acceptThread.join();
    if (networkThread.joinable()) networkThread.join();
    for (std::thread& t : clientThreads) {
        if (t.joinable()) t.join();
    }

    WSACleanup();
}

bool BottomLayer::HostGame(int port) {
    isHost = true;
    isRunning = true;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cout << "Host socket creation failed.\n";
        return false;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(port));

    if (bind(sock, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    if (listen(sock, MAX_REMOTE_CLIENTS) == SOCKET_ERROR) {
        std::cout << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    listenSocket = sock;
    std::cout << "Hosting on port " << port << ". Up to " << MAX_REMOTE_CLIENTS << " clients may join.\n";
    acceptThread = std::thread(&BottomLayer::AcceptLoop, this, static_cast<uintptr_t>(sock));
    return true;
}

bool BottomLayer::ConnectToGame(const std::string& virtualIp, int port) {
    isHost = false;
    isRunning = true;

    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        std::cout << "Client socket creation failed.\n";
        return false;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, virtualIp.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(static_cast<u_short>(port));

    std::cout << "Connecting to " << virtualIp << ":" << port << "...\n";
    if (connect(connectSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Connection failed: " << WSAGetLastError() << "\n";
        closesocket(connectSocket);
        return false;
    }

    activeSocket = static_cast<uintptr_t>(connectSocket);
    std::cout << "Connected to host. Waiting for player assignment.\n";
    networkThread = std::thread(&BottomLayer::ClientReadLoop, this, activeSocket, false);
    return true;
}

void BottomLayer::AcceptLoop(uintptr_t listenSock) {
    while (isRunning) {
        SOCKET clientSocket = accept((SOCKET)listenSock, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) std::cout << "accept() failed: " << WSAGetLastError() << "\n";
            break;
        }

        uint32_t assignedId = 0;
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (clientSockets.size() >= MAX_REMOTE_CLIENTS) {
                closesocket(clientSocket);
                continue;
            }
            clientSockets.push_back(static_cast<uintptr_t>(clientSocket));
            assignedId = static_cast<uint32_t>(clientSockets.size()); // 1, 2, 3
        }

        AssignPlayerPacket assign{};
        assign.playerId = assignedId;
        SendFramed(static_cast<uintptr_t>(clientSocket), SerializeAssignPlayer(assign));
        std::cout << "Client joined as player " << assignedId << ".\n";

        clientThreads.emplace_back(&BottomLayer::ClientReadLoop, this, static_cast<uintptr_t>(clientSocket), true);
    }
}

void BottomLayer::ClientReadLoop(uintptr_t socketHandle, bool relayToClients) {
    while (isRunning && socketHandle != 0) {
        uint32_t payloadSize = 0;
        if (!RecvExact((SOCKET)socketHandle, reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize))) {
            break;
        }

        if (payloadSize == 0 || payloadSize > 4096) {
            break;
        }

        std::string payload(payloadSize, '\0');
        if (!RecvExact((SOCKET)socketHandle, &payload[0], static_cast<int>(payloadSize))) {
            break;
        }

        PushIncoming(payload);

        // Host acts as the input relay: every client input is forwarded to every client.
        if (isHost && relayToClients && PeekPacketType(payload) == PacketType::CLIENT_INPUT) {
            BroadcastFramed(payload);
        }
    }

    closesocket((SOCKET)socketHandle);
}

void BottomLayer::SendNetworkData(const std::string& payload) {
    if (payload.empty()) return;

    if (isHost) {
        BroadcastFramed(payload);
    }
    else if (activeSocket != 0) {
        SendFramed(activeSocket, payload);
    }
}

void BottomLayer::SendFramed(uintptr_t socketHandle, const std::string& payload) {
    if (socketHandle == 0 || payload.empty()) return;

    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    send((SOCKET)socketHandle, reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize), 0);
    send((SOCKET)socketHandle, payload.data(), static_cast<int>(payload.size()), 0);
}

void BottomLayer::BroadcastFramed(const std::string& payload, uintptr_t exceptSocket) {
    std::lock_guard<std::mutex> lock(socketMutex);
    for (uintptr_t socketHandle : clientSockets) {
        if (socketHandle != 0 && socketHandle != exceptSocket) {
            SendFramed(socketHandle, payload);
        }
    }
}

void BottomLayer::PushIncoming(const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        incomingDataQueue.push(payload);
    }
    cv.notify_one();
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

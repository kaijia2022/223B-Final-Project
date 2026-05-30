#include "BottomLayer.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr uint16_t DEFAULT_HOST_PEER_PORT = 9000;
constexpr int MAX_PAYLOAD_SIZE = 8192;
constexpr int MAX_LOBBY_CLIENTS = MAX_PLAYERS - 1;

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

BottomLayer::BottomLayer()
    : isRunning(false), isHost(false), hasAssignment(false), gameStarted(false),
      localPlayerId(UINT32_MAX), localPeerPort(0),
      outboundDelayRng(std::random_device{}()), outboundDelayEnabled(false),
      outboundDelayMinMs(0), outboundDelayMaxMs(0), outboundSenderStop(false) {
    WSADATA wsaData;
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cout << "WSAStartup failed: " << result << "\n";
    }

    // Dedicated delayed-send worker. This makes artificial outbound delay
    // independent of the render/simulation loop, so a rollback freeze does not
    // accidentally stretch the delay far beyond the chosen range.
    outboundSenderThread = std::thread(&BottomLayer::OutboundSenderLoop, this);
}

BottomLayer::~BottomLayer() {
    isRunning = false;
    outboundSenderStop = true;
    outboundDelayCv.notify_all();
    if (outboundSenderThread.joinable()) outboundSenderThread.join();

    auto closeIfValid = [](uintptr_t& s) {
        if (s != 0) {
            closesocket((SOCKET)s);
            s = 0;
        }
    };

    closeIfValid(lobbyListenSocket);
    closeIfValid(lobbySocket);
    closeIfValid(peerListenSocket);

    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        for (uintptr_t s : lobbyClientSockets) {
            if (s != 0) closesocket((SOCKET)s);
        }
        lobbyClientSockets.clear();
    }

    {
        std::lock_guard<std::mutex> lock(peerSocketMutex);
        for (auto& kv : peerSockets) {
            if (kv.second != 0) closesocket((SOCKET)kv.second);
        }
        peerSockets.clear();
    }

    if (lobbyAcceptThread.joinable()) lobbyAcceptThread.join();
    if (lobbyReadThread.joinable()) lobbyReadThread.join();
    if (peerAcceptThread.joinable()) peerAcceptThread.join();
    for (std::thread& t : workerThreads) {
        if (t.joinable()) t.join();
    }

    WSACleanup();
}

bool BottomLayer::HostGame(int lobbyPort) {
    isHost = true;
    isRunning = true;
    localPlayerId = 0;
    hasAssignment = true;
    gameStarted = false;

    if (!StartPeerListener(DEFAULT_HOST_PEER_PORT)) {
        std::cout << "Host failed to start peer listener on port " << DEFAULT_HOST_PEER_PORT << ".\n";
        return false;
    }

    std::memset(knownPeers, 0, sizeof(knownPeers));
    knownPeers[0].playerId = 0;
    knownPeers[0].active = true;
    knownPeers[0].port = localPeerPort;
    CopyIp(knownPeers[0].ip, "HOST");

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cout << "Host lobby socket creation failed.\n";
        return false;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(lobbyPort));

    if (bind(sock, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "lobby bind() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    if (listen(sock, MAX_LOBBY_CLIENTS) == SOCKET_ERROR) {
        std::cout << "lobby listen() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    lobbyListenSocket = static_cast<uintptr_t>(sock);
    std::cout << "Host lobby listening on " << lobbyPort << ", peer port " << localPeerPort << ".\n";
    lobbyAcceptThread = std::thread(&BottomLayer::LobbyAcceptLoop, this, lobbyListenSocket);
    return true;
}

bool BottomLayer::ConnectToGame(const std::string& hostIp, int lobbyPort) {
    isHost = false;
    isRunning = true;
    hasAssignment = false;
    gameStarted = false;
    lobbyHostIp = hostIp;

    if (!StartPeerListener(0)) {
        std::cout << "Client failed to start peer listener.\n";
        return false;
    }

    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        std::cout << "Client lobby socket creation failed.\n";
        return false;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, hostIp.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(static_cast<u_short>(lobbyPort));

    std::cout << "Connecting to host lobby " << hostIp << ":" << lobbyPort
              << ", local peer port " << localPeerPort << "...\n";

    if (connect(connectSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Lobby connection failed: " << WSAGetLastError() << "\n";
        closesocket(connectSocket);
        return false;
    }

    lobbySocket = static_cast<uintptr_t>(connectSocket);

    JoinRequestPacket join{};
    join.peerPort = localPeerPort;
    SendFramed(lobbySocket, SerializeJoinRequest(join));

    lobbyReadThread = std::thread(&BottomLayer::LobbyClientReadLoop, this, lobbySocket);
    return true;
}

bool BottomLayer::StartPeerListener(uint16_t requestedPort) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(requestedPort);

    if (bind(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cout << "peer bind() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    if (listen(sock, MAX_PLAYERS - 1) == SOCKET_ERROR) {
        std::cout << "peer listen() failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    peerListenSocket = static_cast<uintptr_t>(sock);
    localPeerPort = GetBoundPort(peerListenSocket);
    peerAcceptThread = std::thread(&BottomLayer::PeerAcceptLoop, this, peerListenSocket);
    return true;
}

uint16_t BottomLayer::GetBoundPort(uintptr_t socketHandle) const {
    sockaddr_in addr = {};
    int len = sizeof(addr);
    if (getsockname((SOCKET)socketHandle, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

void BottomLayer::LobbyAcceptLoop(uintptr_t listenSock) {
    while (isRunning) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept((SOCKET)listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) std::cout << "lobby accept() failed: " << WSAGetLastError() << "\n";
            break;
        }

        char ipStr[PEER_IP_STRING_SIZE] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

        std::string payload;
        if (!RecvFramed(static_cast<uintptr_t>(clientSocket), payload)) {
            closesocket(clientSocket);
            continue;
        }

        JoinRequestPacket join{};
        if (!DeserializeJoinRequest(payload, join)) {
            closesocket(clientSocket);
            continue;
        }

        HostHandleJoin(static_cast<uintptr_t>(clientSocket), ipStr, join);
    }
}

void BottomLayer::HostHandleJoin(uintptr_t clientSocket, const std::string& clientIp, const JoinRequestPacket& join) {
    uint32_t assignedId = UINT32_MAX;

    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        for (uint32_t i = 1; i < MAX_PLAYERS; ++i) {
            if (!knownPeers[i].active) {
                assignedId = i;
                break;
            }
        }

        if (assignedId == UINT32_MAX) {
            closesocket((SOCKET)clientSocket);
            return;
        }

        lobbyClientSockets.push_back(clientSocket);
        knownPeers[assignedId].playerId = assignedId;
        knownPeers[assignedId].port = join.peerPort;
        knownPeers[assignedId].active = true;
        CopyIp(knownPeers[assignedId].ip, clientIp);
    }

    AssignPlayerPacket assign{};
    assign.playerId = assignedId;
    SendFramed(clientSocket, SerializeAssignPlayer(assign));

    std::cout << "Player " << assignedId << " joined from " << clientIp
              << ":" << join.peerPort << ".\n";

    BroadcastPeerListFromHost();

    // The host also uses the peer list to create direct P2P sockets to clients.
    PeerListPacket list{};
    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        list.type = PacketType::PEER_LIST;
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
            list.peers[i] = knownPeers[i];
            if (knownPeers[i].active) list.peerCount++;
        }
    }
    ApplyPeerList(list);

    // Keep the lobby socket alive only for lobby packets such as START_GAME and PEER_LIST.
}

void BottomLayer::BroadcastPeerListFromHost() {
    PeerListPacket list{};
    list.type = PacketType::PEER_LIST;

    std::vector<uintptr_t> clients;
    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
            list.peers[i] = knownPeers[i];
            if (knownPeers[i].active) list.peerCount++;
        }
        clients = lobbyClientSockets;
    }

    const std::string payload = SerializePeerList(list);
    for (uintptr_t s : clients) {
        SendFramed(s, payload);
    }
}

void BottomLayer::LobbyClientReadLoop(uintptr_t socketHandle) {
    while (isRunning && socketHandle != 0) {
        std::string payload;
        if (!RecvFramed(socketHandle, payload)) break;

        const PacketType type = PeekPacketType(payload);
        if (type == PacketType::ASSIGN_PLAYER) {
            AssignPlayerPacket assign{};
            if (DeserializeAssignPlayer(payload, assign)) {
                localPlayerId = assign.playerId;
                hasAssignment = true;
                std::cout << "Assigned local player id " << assign.playerId << ".\n";
            }
        } else if (type == PacketType::PEER_LIST) {
            PeerListPacket list{};
            if (DeserializePeerList(payload, list)) {
                ApplyPeerList(list);
            }
        } else if (type == PacketType::START_GAME) {
            StartGamePacket start{};
            if (DeserializeStartGame(payload, start)) {
                gameStarted = true;
            }
        }
    }
}

void BottomLayer::ApplyPeerList(const PeerListPacket& peerList) {
    if (!hasAssignment) return;
    const uint32_t myId = localPlayerId.load();

    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
            knownPeers[i] = peerList.peers[i];
        }
        if (myId < MAX_PLAYERS) {
            knownPeers[myId].playerId = myId;
            knownPeers[myId].active = true;
            knownPeers[myId].port = localPeerPort;
        }
    }

    for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
        PeerInfo peer = peerList.peers[i];
        if (!peer.active || peer.playerId >= MAX_PLAYERS || peer.playerId == myId) continue;

        if (std::strcmp(peer.ip, "HOST") == 0 && !lobbyHostIp.empty()) {
            CopyIp(peer.ip, lobbyHostIp);
        }

        // Deterministic connection rule: lower playerId actively connects to
        // higher playerId. Higher IDs wait for incoming peer connections.
        if (myId < peer.playerId) {
            ConnectToPeer(peer);
        }
    }
}

bool BottomLayer::ConnectToPeer(const PeerInfo& peer) {
    {
        std::lock_guard<std::mutex> lock(peerSocketMutex);
        if (peerSockets.find(peer.playerId) != peerSockets.end()) return true;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, peer.ip, &addr.sin_addr);
    addr.sin_port = htons(peer.port);

    if (connect(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cout << "P2P connect to P" << peer.playerId << " at " << peer.ip << ":" << peer.port
                  << " failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    PeerHelloPacket hello{};
    hello.playerId = localPlayerId;
    SendFramed(static_cast<uintptr_t>(sock), SerializePeerHello(hello));

    RegisterPeerSocket(peer.playerId, static_cast<uintptr_t>(sock));
    std::cout << "Connected directly to peer P" << peer.playerId << ".\n";
    return true;
}

void BottomLayer::PeerAcceptLoop(uintptr_t listenSock) {
    while (isRunning) {
        SOCKET peerSocket = accept((SOCKET)listenSock, nullptr, nullptr);
        if (peerSocket == INVALID_SOCKET) {
            if (isRunning) std::cout << "peer accept() failed: " << WSAGetLastError() << "\n";
            break;
        }

        std::string helloPayload;
        if (!RecvFramed(static_cast<uintptr_t>(peerSocket), helloPayload)) {
            closesocket(peerSocket);
            continue;
        }

        PeerHelloPacket hello{};
        if (!DeserializePeerHello(helloPayload, hello) || hello.playerId >= MAX_PLAYERS) {
            closesocket(peerSocket);
            continue;
        }

        RegisterPeerSocket(hello.playerId, static_cast<uintptr_t>(peerSocket));
        std::cout << "Accepted direct peer P" << hello.playerId << ".\n";
    }
}

void BottomLayer::RegisterPeerSocket(uint32_t peerId, uintptr_t socketHandle) {
    {
        std::lock_guard<std::mutex> lock(peerSocketMutex);
        auto it = peerSockets.find(peerId);
        if (it != peerSockets.end()) {
            closesocket((SOCKET)socketHandle);
            return;
        }
        peerSockets[peerId] = socketHandle;
    }

    workerThreads.emplace_back(&BottomLayer::PeerReadLoop, this, socketHandle, peerId);
}

void BottomLayer::PeerReadLoop(uintptr_t socketHandle, uint32_t fromPlayerId) {
    while (isRunning && socketHandle != 0) {
        std::string payload;
        if (!RecvFramed(socketHandle, payload)) break;

        // Gameplay packets arrive directly from peers. No host relay.
        if (PeekPacketType(payload) == PacketType::CLIENT_INPUT ||
            PeekPacketType(payload) == PacketType::GAME_STATE_RESYNC) {
            PushIncoming(payload);
        }
    }

    closesocket((SOCKET)socketHandle);
    std::lock_guard<std::mutex> lock(peerSocketMutex);
    auto it = peerSockets.find(fromPlayerId);
    if (it != peerSockets.end() && it->second == socketHandle) {
        peerSockets.erase(it);
    }
}

void BottomLayer::BroadcastStartGame() {
    gameStarted = true;
    if (!isHost) return;

    StartGamePacket start{};
    const std::string payload = SerializeStartGame(start);

    std::vector<uintptr_t> clients;
    {
        std::lock_guard<std::mutex> lock(lobbyMutex);
        clients = lobbyClientSockets;
    }

    for (uintptr_t s : clients) {
        SendFramed(s, payload);
    }
}

void BottomLayer::SendNetworkData(const std::string& payload) {
    BroadcastToPeers(payload);
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

    std::lock_guard<std::mutex> lock(outboundDelayMutex);
    outboundDelayQueue.clear();
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

void BottomLayer::QueueDelayedOutbound(uint32_t targetPlayerId, const std::string& payload) {
    if (payload.empty()) return;

    DelayedOutboundPacket packet;
    packet.targetPlayerId = targetPlayerId;
    packet.payload = payload;

    {
        std::lock_guard<std::mutex> lock(outboundDelayMutex);
        packet.sendTime = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(ComputeOutboundDelayMs());
        outboundDelayQueue.push_back(std::move(packet));
    }

    outboundDelayCv.notify_one();
}

void BottomLayer::FlushDelayedOutboundPackets() {
    // Kept for compatibility with older Game.cpp patches. A dedicated sender
    // thread now performs timed flushing. This method only wakes the sender so
    // packets that are already due can be released promptly.
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
            } else {
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
                } else {
                    ++it;
                }
            }
        }

        for (const DelayedOutboundPacket& packet : readyPackets) {
            SendToPeerImmediate(packet.targetPlayerId, packet.payload);
        }
    }
}

void BottomLayer::BroadcastToPeers(const std::string& payload) {
    if (payload.empty()) return;

    if (outboundDelayEnabled) {
        // Snapshot the current peer IDs now. A peer that connects later should
        // not receive old delayed input packets from before it was connected.
        std::vector<uint32_t> peerIds;
        {
            std::lock_guard<std::mutex> lock(peerSocketMutex);
            for (const auto& kv : peerSockets) {
                if (kv.second != 0) peerIds.push_back(kv.first);
            }
        }

        for (uint32_t peerId : peerIds) {
            QueueDelayedOutbound(peerId, payload);
        }
        return;
    }

    BroadcastToPeersImmediate(payload);
}

void BottomLayer::BroadcastToPeersImmediate(const std::string& payload) {
    if (payload.empty()) return;

    std::vector<uintptr_t> sockets;
    {
        std::lock_guard<std::mutex> lock(peerSocketMutex);
        for (auto& kv : peerSockets) {
            if (kv.second != 0) sockets.push_back(kv.second);
        }
    }

    for (uintptr_t s : sockets) {
        SendFramed(s, payload);
    }
}

void BottomLayer::SendToPeer(uint32_t peerId, const std::string& payload) {
    if (payload.empty()) return;

    if (outboundDelayEnabled) {
        QueueDelayedOutbound(peerId, payload);
        return;
    }

    SendToPeerImmediate(peerId, payload);
}

void BottomLayer::SendToPeerImmediate(uint32_t peerId, const std::string& payload) {
    uintptr_t socketHandle = 0;
    {
        std::lock_guard<std::mutex> lock(peerSocketMutex);
        auto it = peerSockets.find(peerId);
        if (it != peerSockets.end()) socketHandle = it->second;
    }

    if (socketHandle != 0) SendFramed(socketHandle, payload);
}

bool BottomLayer::SendFramed(uintptr_t socketHandle, const std::string& payload) {
    if (socketHandle == 0 || payload.empty()) return false;

    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    const int headerSent = send((SOCKET)socketHandle, reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize), 0);
    if (headerSent != sizeof(payloadSize)) return false;

    int totalSent = 0;
    while (totalSent < static_cast<int>(payload.size())) {
        int sent = send((SOCKET)socketHandle, payload.data() + totalSent, static_cast<int>(payload.size()) - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

bool BottomLayer::RecvFramed(uintptr_t socketHandle, std::string& outPayload) {
    uint32_t payloadSize = 0;
    if (!RecvExact((SOCKET)socketHandle, reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize))) {
        return false;
    }

    if (payloadSize == 0 || payloadSize > MAX_PAYLOAD_SIZE) return false;

    outPayload.assign(payloadSize, '\0');
    return RecvExact((SOCKET)socketHandle, &outPayload[0], static_cast<int>(payloadSize));
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

bool BottomLayer::HasPlayerAssignment() const { return hasAssignment; }
uint32_t BottomLayer::GetLocalPlayerId() const { return localPlayerId; }
bool BottomLayer::IsGameStarted() const { return gameStarted; }
uint32_t BottomLayer::GetConnectedPeerCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(peerSocketMutex));
    return static_cast<uint32_t>(peerSockets.size());
}
uint16_t BottomLayer::GetLocalPeerPort() const { return localPeerPort; }

uint32_t BottomLayer::GetKnownPlayerMask() const {
    uint32_t mask = 0;
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(lobbyMutex));
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
            if (knownPeers[i].active && knownPeers[i].playerId < MAX_PLAYERS) {
                mask |= (1u << knownPeers[i].playerId);
            }
        }
    }

    const uint32_t myId = localPlayerId.load();
    if (myId < MAX_PLAYERS) mask |= (1u << myId);
    return mask;
}

void BottomLayer::InjectKeyDown(int keycode) { injectedKeyStates[keycode] = true; }
void BottomLayer::InjectKeyUp(int keycode) { injectedKeyStates[keycode] = false; }
bool BottomLayer::IsActionPressed(int keycode) {
    auto it = injectedKeyStates.find(keycode);
    return it != injectedKeyStates.end() && it->second;
}

void BottomLayer::CopyIp(char dst[PEER_IP_STRING_SIZE], const std::string& ip) {
    std::memset(dst, 0, PEER_IP_STRING_SIZE);
    const size_t copyLen = min(ip.size(), static_cast<size_t>(PEER_IP_STRING_SIZE - 1));
    if (copyLen > 0) {
        std::memcpy(dst, ip.data(), copyLen);
    }
}

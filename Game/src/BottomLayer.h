#pragma once
#include "Engine.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class BottomLayer {
public:
    BottomLayer();
    ~BottomLayer();

    // HostGame starts a lobby listener and a peer listener. The host is player 0.
    bool HostGame(int lobbyPort);

    // ConnectToGame starts this client's peer listener, joins the host lobby,
    // receives player assignment + peer list, then forms direct P2P sockets.
    bool ConnectToGame(const std::string& hostIp, int lobbyPort);

    // During gameplay this broadcasts only to direct peers. The host no longer
    // relays client gameplay messages.
    void SendNetworkData(const std::string& payload);
    void BroadcastToPeers(const std::string& payload);
    void SendToPeer(uint32_t peerId, const std::string& payload);

    // Host calls this when the lobby is ready.
    void BroadcastStartGame();

    bool HasIncomingData();
    std::string GetNextNetworkMessage();

    bool HasPlayerAssignment() const;
    uint32_t GetLocalPlayerId() const;
    bool IsGameStarted() const;
    uint32_t GetConnectedPeerCount() const;
    uint16_t GetLocalPeerPort() const;
    uint32_t GetKnownPlayerMask() const;

    void InjectKeyDown(int keycode);
    void InjectKeyUp(int keycode);
    bool IsActionPressed(int keycode);

private:
    std::atomic<bool> isRunning;
    bool isHost;

    std::atomic<bool> hasAssignment;
    std::atomic<bool> gameStarted;
    std::atomic<uint32_t> localPlayerId;
    std::atomic<uint16_t> localPeerPort;

    std::string lobbyHostIp;

    uintptr_t lobbyListenSocket = 0;
    uintptr_t lobbySocket = 0;
    uintptr_t peerListenSocket = 0;

    std::mutex lobbyMutex;
    std::vector<uintptr_t> lobbyClientSockets;
    PeerInfo knownPeers[MAX_PLAYERS] = {};

    std::mutex peerSocketMutex;
    std::unordered_map<uint32_t, uintptr_t> peerSockets;

    std::queue<std::string> incomingDataQueue;
    std::mutex queueMutex;
    std::condition_variable cv;

    std::thread lobbyAcceptThread;
    std::thread lobbyReadThread;
    std::thread peerAcceptThread;
    std::vector<std::thread> workerThreads;

    std::unordered_map<int, bool> injectedKeyStates;

    bool StartPeerListener(uint16_t requestedPort);
    uint16_t GetBoundPort(uintptr_t socketHandle) const;

    void LobbyAcceptLoop(uintptr_t listenSock);
    void LobbyClientReadLoop(uintptr_t socketHandle);
    void HostHandleJoin(uintptr_t clientSocket, const std::string& clientIp, const JoinRequestPacket& join);
    void BroadcastPeerListFromHost();
    void ApplyPeerList(const PeerListPacket& peerList);

    void PeerAcceptLoop(uintptr_t listenSock);
    void PeerReadLoop(uintptr_t socketHandle, uint32_t fromPlayerId);
    bool ConnectToPeer(const PeerInfo& peer);
    void RegisterPeerSocket(uint32_t peerId, uintptr_t socketHandle);

    void PushIncoming(const std::string& payload);
    bool SendFramed(uintptr_t socketHandle, const std::string& payload);
    bool RecvFramed(uintptr_t socketHandle, std::string& outPayload);

    static void CopyIp(char dst[PEER_IP_STRING_SIZE], const std::string& ip);
};

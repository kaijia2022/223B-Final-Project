#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <cstdint>

class BottomLayer {
public:
    BottomLayer();
    ~BottomLayer();

    bool HostGame(int port);
    bool ConnectToGame(const std::string& virtualIp, int port);

    // Host: broadcasts to all connected clients.
    // Client: sends to the host.
    void SendNetworkData(const std::string& payload);

    bool HasIncomingData();
    std::string GetNextNetworkMessage();

    void InjectKeyDown(int keycode);
    void InjectKeyUp(int keycode);
    bool IsActionPressed(int keycode);

private:
    bool isHost;
    std::atomic<bool> isRunning;

    uintptr_t listenSocket = 0;
    uintptr_t activeSocket = 0; // Client's connection to host.

    std::mutex socketMutex;
    std::vector<uintptr_t> clientSockets;

    std::queue<std::string> incomingDataQueue;
    std::mutex queueMutex;
    std::condition_variable cv;

    std::thread acceptThread;
    std::vector<std::thread> clientThreads;
    std::thread networkThread;

    std::unordered_map<int, bool> injectedKeyStates;

    void AcceptLoop(uintptr_t listenSock);
    void ClientReadLoop(uintptr_t socketHandle, bool relayToClients);
    void PushIncoming(const std::string& payload);
    void SendFramed(uintptr_t socketHandle, const std::string& payload);
    void BroadcastFramed(const std::string& payload, uintptr_t exceptSocket = 0);
};

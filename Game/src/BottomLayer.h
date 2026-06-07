#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <random>

// Note: If using Raylib, include it here or map its keycodes.
// #include "raylib.h" 

class BottomLayer {
public:
    BottomLayer();
    ~BottomLayer();

    // --- 1. NETWORKING INTERFACE ---
    // Start listening for connections (Host)
    bool HostGame(int port);
    // Connect to a host via Virtual IP (Join)
    bool ConnectToGame(const std::string& virtualIp, int port);

    // --- 2. DATA INTERFACE (For the Middle Layer) ---
    // Accept data from middle layer to send over TCP
    void SendNetworkData(const std::string& payload);
    // Optional testing tool: delay this instance's outbound network packets.
    // Press F1 in Game.cpp to toggle this at runtime.
    void SetOutboundDelayRange(int minDelayMs, int maxDelayMs);
    void ClearOutboundDelay();
    bool IsOutboundDelayEnabled() const;
    int GetOutboundDelayMinMs() const;
    int GetOutboundDelayMaxMs() const;
    void FlushDelayedOutboundPackets();

    // Middle layer calls this every frame to process new network events
    bool HasIncomingData();
    std::string GetNextNetworkMessage();

    // --- 3. INPUT INJECTION & TESTING ---
    // Automated testing scripts call these
    void InjectKeyDown(int keycode);
    void InjectKeyUp(int keycode);

    // Middle layer calls this instead of Raylib's IsKeyDown()
    bool IsActionPressed(int keycode);

private:
    // Network state
    bool isRunning;
    bool isHost;

    uintptr_t activeSocket = 0;
    uintptr_t listenSocketHandle = 0;

    // Host-side client sockets. The vector index is not the player id;
    // player ids are assigned as clients connect.
    std::vector<uintptr_t> clientSockets;
    std::vector<std::thread> clientThreads;
    std::mutex socketMutex;

    struct DelayedOutboundPacket {
        uintptr_t socketHandle = 0;
        std::string payload;
        std::chrono::steady_clock::time_point sendTime;
    };

    mutable std::mutex outboundDelayMutex;
    std::deque<DelayedOutboundPacket> outboundDelayQueue;
    std::mt19937 outboundDelayRng;
    std::atomic<bool> outboundDelayEnabled;
    std::atomic<int> outboundDelayMinMs;
    std::atomic<int> outboundDelayMaxMs;
    std::atomic<bool> outboundSenderStop;
    std::condition_variable outboundDelayCv;
    std::thread outboundSenderThread;

    // Thread-safe message queue for the game loop
    std::queue<std::string> incomingDataQueue;
    std::mutex queueMutex;

    // Background thread so TCP waiting doesn't freeze the game
    std::thread networkThread;
    void NetworkWorkerLoop();
    void ClientReceiveLoop(uintptr_t socketHandle, uint32_t assignedPlayerId);

    void SendImmediate(uintptr_t socketHandle, const std::string& payload);
    void QueueDelayedOutbound(uintptr_t socketHandle, const std::string& payload);
    int ComputeOutboundDelayMs();
    void OutboundSenderLoop();

    // Input state
    std::unordered_map<int, bool> injectedKeyStates;
};

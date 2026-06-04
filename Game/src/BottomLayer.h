#pragma once
#include "Engine.h"
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstdint>
#include <atomic>

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

    // The host is always player 0. Clients receive player IDs 1..MAX_PLAYERS-1.
    uint32_t GetLocalPlayerId() const;
    bool IsPlayerConnected(uint32_t playerId) const;

    // --- 2. DATA INTERFACE (For the Middle Layer) ---
    // Accept data from middle layer to send over TCP.
    // Host broadcasts to every connected client; client sends to the host.
    void SendNetworkData(const std::string& payload);
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
    std::atomic_bool isRunning;
    bool isHost;

    uintptr_t activeSocket = 0;       // Client mode: socket connected to host
    uintptr_t listenSocket = 0;       // Host mode: listening socket

    mutable std::mutex socketMutex;
    std::array<uintptr_t, MAX_PLAYERS> playerSockets{}; // Host mode: socket per player slot; player 0 is host/no socket.
    std::array<bool, MAX_PLAYERS> connectedPlayers{};
    uint32_t localPlayerId;

    // Thread-safe message queue for the game loop
    std::queue<std::string> incomingDataQueue;
    std::mutex queueMutex;

    // Background threads so TCP waiting doesn't freeze the game
    std::thread networkThread;
    std::vector<std::thread> clientThreads;
    void NetworkWorkerLoop(uintptr_t socketHandle, uint32_t playerId);
    void HostAcceptLoop(uintptr_t listenSocketHandle);
    uint32_t ReserveClientSlot();
    void MarkPlayerDisconnected(uint32_t playerId, uintptr_t socketHandle);

    // Input state
    std::unordered_map<int, bool> injectedKeyStates;
};

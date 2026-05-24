#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

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

    // Thread-safe message queue for the game loop
    std::queue<std::string> incomingDataQueue;
    std::mutex queueMutex;

    // Background thread so TCP waiting doesn't freeze the game
    std::thread networkThread;  
    void NetworkWorkerLoop();

    // Input state
    std::unordered_map<int, bool> injectedKeyStates;
};
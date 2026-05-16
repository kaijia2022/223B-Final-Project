#include "BottomLayer.h"
#include <iostream>

BottomLayer::BottomLayer() : isRunning(false), isHost(false) {}

BottomLayer::~BottomLayer() {
    isRunning = false;
    if (networkThread.joinable()) {
        networkThread.join();
    }
    // TODO: Close sockets here
}

// ==========================================
// NETWORKING: HOST & CONNECT
// ==========================================
bool BottomLayer::HostGame(int port) {
    isHost = true;
    isRunning = true;

    // TODO: Initialize Winsock, create TCP socket, bind(port), listen()
    std::cout << "Hosting server on port " << port << "...\n";

    // Start background thread to handle connections and receive data
    networkThread = std::thread(&BottomLayer::NetworkWorkerLoop, this);
    return true;
}

bool BottomLayer::ConnectToGame(const std::string& virtualIp, int port) {
    isHost = false;
    isRunning = true;

    // TODO: Initialize Winsock, create TCP socket, connect(virtualIp, port)
    std::cout << "Connecting to " << virtualIp << ":" << port << "...\n";

    // Start background thread to listen to the server
    networkThread = std::thread(&BottomLayer::NetworkWorkerLoop, this);
    return true;
}

// ==========================================
// THREAD-SAFE DATA INTERFACE
// ==========================================
void BottomLayer::SendNetworkData(const std::string& payload) {
    // TODO: send(socket, payload.c_str(), payload.length(), 0)
    // If isHost, iterate through connected client sockets and send to all
    // If not isHost, send to the single server socket
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

// Background thread loop (DO NOT call Raylib functions in here!)
void BottomLayer::NetworkWorkerLoop() {
    while (isRunning) {
        // TODO: recv(socket, buffer, size, 0)
        // This is a blocking call, which is why it's on a separate thread.

        std::string mockReceivedData = "Player_2_Move_Right"; // Mocked data

        if (!mockReceivedData.empty()) {
            // Safely push to the queue so the Middle Layer can read it next frame
            std::lock_guard<std::mutex> lock(queueMutex);
            incomingDataQueue.push(mockReceivedData);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ==========================================
// INPUT INJECTION & ABSTRACTION
// ==========================================
void BottomLayer::InjectKeyDown(int keycode) {
    injectedKeyStates[keycode] = true;
}

void BottomLayer::InjectKeyUp(int keycode) {
    injectedKeyStates[keycode] = false;
}

bool BottomLayer::IsActionPressed(int keycode) {
    // 1. Check if an automated test is holding the key down
    if (injectedKeyStates.find(keycode) != injectedKeyStates.end()) {
        if (injectedKeyStates[keycode] == true) {
            return true;
        }
    }

    // 2. Fallback to actual physical hardware input (Raylib)
    // return IsKeyDown(keycode); 
    return false; // Mocked for now
}
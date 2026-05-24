#include "BottomLayer.h"
#include <iostream>

// Include Winsock
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")

BottomLayer::BottomLayer() : isRunning(false), isHost(false), activeSocket(0) {
    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

BottomLayer::~BottomLayer() {
    isRunning = false;
    if (activeSocket != 0) {
        closesocket((SOCKET)activeSocket);
    }
    if (networkThread.joinable()) {
        networkThread.join();
    }
    WSACleanup();
}

bool BottomLayer::HostGame(int port) {
    isHost = true;
    isRunning = true;

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Listen on all adapters (Localhost & Hamachi)
    serverAddr.sin_port = htons(port);

    bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
    listen(listenSocket, 1); // Listen for 1 client for now

    std::cout << "Hosting server on port " << port << "... Waiting for client.\n";

    // Start background thread to handle connections and receive data
    networkThread = std::thread([this, listenSocket]() {
        // Block until the client joins
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        this->activeSocket = clientSocket;
        closesocket(listenSocket); // Stop listening for others once we have our 1 client

        std::cout << "Client connected!\n";
        this->NetworkWorkerLoop();
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

    std::cout << "Connected to Host!\n";
    this->activeSocket = connectSocket;

    // Start background thread to listen to the server
    networkThread = std::thread(&BottomLayer::NetworkWorkerLoop, this);
    return true;
}

void BottomLayer::SendNetworkData(const std::string& payload) {
    if (activeSocket != 0) {
        // Send the raw binary data over TCP
        send((SOCKET)activeSocket, payload.data(), (int)payload.size(), 0);
    }
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
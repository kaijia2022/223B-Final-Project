#include "raylib.h"
#include "ConnectForm.h"
#include "Engine.h"
#include "TopLayer.h"
#include "BottomLayer.h"
#include <string>

bool CheckPlayerCoinCollision(float px, float py, float cx, float cy) {
    float distanceSq = (px - cx) * (px - cx) + (py - cy) * (py - cy);
    return distanceSq < 625.0f;
}

int main() {
    InitWindow(800, 600, "Gold Rush - Multiplayer Ready");
    SetTargetFPS(30);

    std::string targetIp = "";
    NetworkRole role = NetworkRole::NONE;

    RunConnectionScreen(targetIp, role);
    if (role == NetworkRole::NONE) {
        CloseWindow();
        return 0;
    }

    BottomLayer bottomLayer;
    uint32_t myLocalPlayerId = (role == NetworkRole::HOST) ? 0 : 1;

    if (role == NetworkRole::HOST) bottomLayer.HostGame(8080);
    else bottomLayer.ConnectToGame(targetIp, 8080);

    

    GameStatePacket authoritativeState = {};
    authoritativeState.frameNumber = 0;

    //Initialize World Data
    authoritativeState.players[0] = { 0, 200.0f, 250.0f, 0, 0, true }; // Host (Red)
    authoritativeState.players[1] = { 1, 600.0f, 250.0f, 0, 1, true }; // Client (Blue)

    for (int i = 2; i < MAX_PLAYERS; i++) authoritativeState.players[i].active = false;

    float mockCoinPositionsX[5] = { 150, 300, 600, 200, 550 };
    float mockCoinPositionsY[5] = { 120, 400, 200, 350, 420 };
    for (int i = 0; i < MAX_COINS; i++) {
        if (i < 5) {
            authoritativeState.coins[i] = { (uint32_t)i, mockCoinPositionsX[i], mockCoinPositionsY[i], true };
        }
        else {
            authoritativeState.coins[i].active = false;
        }
    }

    // MAIN GAME LOOP
    while (!WindowShouldClose()) {
        authoritativeState.frameNumber++;

        float moveSpeed = 4.0f;

        //save most recent action, and resend if requested

        // 1. GATHER LOCAL INPUT
        ClientInputPacket localInput = { PacketType::CLIENT_INPUT, myLocalPlayerId, authoritativeState.frameNumber, 0.0f, 0.0f };
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) localInput.moveX = 1.0f;
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))  localInput.moveX = -1.0f;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))  localInput.moveY = 1.0f;
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))    localInput.moveY = -1.0f;

        //sends input
        std::string outData(reinterpret_cast<char*>(&localInput), sizeof(ClientInputPacket));
        bottomLayer.SendNetworkData(outData);

        //receive input from opposing player
        std::string msg = bottomLayer.GetNextNetworkMessage();
        size_t offset = 0;

        ClientInputPacket remoteInput = (ClientInputPacket) msg;
        PacketType type = static_cast<PacketType>(msg[offset]);
        if (type == PacketType::CLIENT_INPUT && offset + sizeof(GameStatePacket) <= msg.size()) {

        }
        else {
            //request most recent input if it's not client_input
            break;
        }

   

        // ==========================================================
        // SERVER (HOST) LOGIC
        // ==========================================================
            

            // Apply Host's local input immediately
            authoritativeState.players[0].x += localInput.moveX * moveSpeed;
            authoritativeState.players[0].y += localInput.moveY * moveSpeed;

            // Process Client Inputs from Network Queue
            while (bottomLayer.HasIncomingData()) {
                std::string msg = bottomLayer.GetNextNetworkMessage();
                size_t offset = 0;

                // Parse potentially merged TCP packets
                while (offset < msg.size()) {
                    PacketType type = static_cast<PacketType>(msg[offset]);
                    if (type == PacketType::CLIENT_INPUT && offset + sizeof(ClientInputPacket) <= msg.size()) {
                        ClientInputPacket clientInput;
                        memcpy(&clientInput, msg.data() + offset, sizeof(ClientInputPacket));

                        // Apply client movement
                        authoritativeState.players[clientInput.playerId].x += clientInput.moveX * moveSpeed;
                        authoritativeState.players[clientInput.playerId].y += clientInput.moveY * moveSpeed;

                        offset += sizeof(ClientInputPacket);
                    }
                    else {
                        break;
                    }
                }

            // Boundary Checks & Collision for ALL players
            for (int pId = 0; pId < 2; pId++) {
                PlayerState& p = authoritativeState.players[pId];
                if (p.x < 65.0f) p.x = 65.0f;  if (p.x > 735.0f) p.x = 735.0f;
                if (p.y < 65.0f) p.y = 65.0f;  if (p.y > 485.0f) p.y = 485.0f;

                for (int cId = 0; cId < MAX_COINS; cId++) {
                    if (authoritativeState.coins[cId].active &&
                        CheckPlayerCoinCollision(p.x, p.y, authoritativeState.coins[cId].x, authoritativeState.coins[cId].y)) {

                        p.score += 10;
                        authoritativeState.coins[cId].active = false;
                        authoritativeState.coins[cId].x = (float)GetRandomValue(100, 700);
                        authoritativeState.coins[cId].y = (float)GetRandomValue(100, 450);
                        authoritativeState.coins[cId].active = true;
                    }
                }
            }

            // Broadcast Authoritative State to Client
            std::string outData(reinterpret_cast<char*>(&authoritativeState), sizeof(GameStatePacket));
            bottomLayer.SendNetworkData(outData);
        }

        // ==========================================================
        // CLIENT LOGIC
        // ==========================================================
        else if (role == NetworkRole::CLIENT) {
            // Send our input to the Host
            std::string outData(reinterpret_cast<char*>(&localInput), sizeof(ClientInputPacket));
            bottomLayer.SendNetworkData(outData);

            // Overwrite local world with Host's World State
            while (bottomLayer.HasIncomingData()) {
                std::string msg = bottomLayer.GetNextNetworkMessage();
                size_t offset = 0;

                while (offset < msg.size()) {
                    PacketType type = static_cast<PacketType>(msg[offset]);
                    if (type == PacketType::GAME_STATE && offset + sizeof(GameStatePacket) <= msg.size()) {
                        memcpy(&authoritativeState, msg.data() + offset, sizeof(GameStatePacket));
                        offset += sizeof(GameStatePacket);
                    }
                    else {
                        break;
                    }
                }
            }
        }

        // ==========================================================
        // RENDER (Both Host and Client do this exactly the same way)
        // ==========================================================
        TopLayer::DrawGame(authoritativeState, myLocalPlayerId);
    }

    CloseWindow();
    return 0;
}
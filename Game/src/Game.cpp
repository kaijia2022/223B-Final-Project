#include "raylib.h"
#include "ConnectForm.h"
#include "Engine.h"
#include "TopLayer.h"
#include "BottomLayer.h"
#include <string>
#include <cstring>

bool CheckPlayerCoinCollision(float px, float py, float cx, float cy) {
    float distanceSq = (px - cx) * (px - cx) + (py - cy) * (py - cy);
    return distanceSq < 625.0f;
}

static void InitializeWorld(GameStatePacket& state) {
    state = {};
    state.type = PacketType::GAME_STATE;
    state.frameNumber = 0;

    const float spawnX[MAX_PLAYERS] = { 200.0f, 600.0f, 200.0f, 600.0f };
    const float spawnY[MAX_PLAYERS] = { 250.0f, 250.0f, 420.0f, 420.0f };

    for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
        state.players[i] = { i, spawnX[i], spawnY[i], 0, static_cast<uint8_t>(i), false };
    }
    state.players[0].active = true; // Host/local player.

    float mockCoinPositionsX[5] = { 150, 300, 600, 200, 550 };
    float mockCoinPositionsY[5] = { 120, 400, 200, 350, 420 };
    for (int i = 0; i < MAX_COINS; i++) {
        if (i < 5) {
            state.coins[i] = { (uint32_t)i, mockCoinPositionsX[i], mockCoinPositionsY[i], true };
        }
        else {
            state.coins[i] = { (uint32_t)i, 0.0f, 0.0f, false };
        }
    }
}

static void ApplyMovement(PlayerState& player, const ClientInputPacket& input, float moveSpeed) {
    player.x += input.moveX * moveSpeed;
    player.y += input.moveY * moveSpeed;
}

static void ClampPlayerToArena(PlayerState& player) {
    if (player.x < 65.0f) player.x = 65.0f;
    if (player.x > 735.0f) player.x = 735.0f;
    if (player.y < 65.0f) player.y = 65.0f;
    if (player.y > 485.0f) player.y = 485.0f;
}

int main() {
    InitWindow(800, 600, "Gold Rush - 4 Player Multiplayer");
    SetTargetFPS(60);

    std::string targetIp = "";
    NetworkRole role = NetworkRole::NONE;

    RunConnectionScreen(targetIp, role);
    if (role == NetworkRole::NONE) {
        CloseWindow();
        return 0;
    }

    BottomLayer bottomLayer;
    bool connected = false;

    if (role == NetworkRole::HOST) {
        connected = bottomLayer.HostGame(8080);
    }
    else {
        connected = bottomLayer.ConnectToGame(targetIp, 8080);
    }

    if (!connected) {
        CloseWindow();
        return 1;
    }

    uint32_t myLocalPlayerId = bottomLayer.GetLocalPlayerId();

    GameStatePacket authoritativeState = {};

    // HOST ONLY: Initialize World Data
    if (role == NetworkRole::HOST) {
        InitializeWorld(authoritativeState);
    }

    bool outboundDelayTestEnabled = false;
    constexpr int OUTBOUND_DELAY_MIN_MS = 150;
    constexpr int OUTBOUND_DELAY_MAX_MS = 450;

    // MAIN GAME LOOP
    while (!WindowShouldClose()) {
        bottomLayer.FlushDelayedOutboundPackets();

        if (IsKeyPressed(KEY_F1)) {
            outboundDelayTestEnabled = !outboundDelayTestEnabled;
            if (outboundDelayTestEnabled) {
                bottomLayer.SetOutboundDelayRange(OUTBOUND_DELAY_MIN_MS, OUTBOUND_DELAY_MAX_MS);
            }
            else {
                bottomLayer.ClearOutboundDelay();
            }
        }
        float moveSpeed = 4.0f;

        // 1. GATHER LOCAL INPUT
        ClientInputPacket localInput = { PacketType::CLIENT_INPUT, myLocalPlayerId, 0.0f, 0.0f };
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) localInput.moveX = 1.0f;
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))  localInput.moveX = -1.0f;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))  localInput.moveY = 1.0f;
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))    localInput.moveY = -1.0f;

        // ==========================================================
        // SERVER (HOST) LOGIC
        // ==========================================================
        if (role == NetworkRole::HOST) {
            authoritativeState.frameNumber++;

            // Activate/deactivate slots based on live connections. The host is always player 0.
            for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
                authoritativeState.players[i].active = bottomLayer.IsPlayerConnected(i);
            }

            // Apply Host's local input immediately.
            ApplyMovement(authoritativeState.players[0], localInput, moveSpeed);

            // Process Client Inputs from Network Queue.
            while (bottomLayer.HasIncomingData()) {
                std::string msg = bottomLayer.GetNextNetworkMessage();
                size_t offset = 0;

                // Parse potentially merged TCP packets.
                while (offset < msg.size()) {
                    PacketType type = static_cast<PacketType>(msg[offset]);
                    if (type == PacketType::CLIENT_INPUT && offset + sizeof(ClientInputPacket) <= msg.size()) {
                        ClientInputPacket clientInput{};
                        std::memcpy(&clientInput, msg.data() + offset, sizeof(ClientInputPacket));

                        // Apply movement only for valid, connected player slots.
                        if (clientInput.playerId < MAX_PLAYERS && authoritativeState.players[clientInput.playerId].active) {
                            ApplyMovement(authoritativeState.players[clientInput.playerId], clientInput, moveSpeed);
                        }

                        offset += sizeof(ClientInputPacket);
                    }
                    else {
                        break;
                    }
                }
            }

            // Boundary Checks & Collision for ALL active players.
            for (uint32_t pId = 0; pId < MAX_PLAYERS; pId++) {
                PlayerState& p = authoritativeState.players[pId];
                if (!p.active) continue;

                ClampPlayerToArena(p);

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

            // Broadcast Authoritative State to every connected client.
            std::string outData(reinterpret_cast<char*>(&authoritativeState), sizeof(GameStatePacket));
            bottomLayer.SendNetworkData(outData);
        }

        // ==========================================================
        // CLIENT LOGIC
        // ==========================================================
        else if (role == NetworkRole::CLIENT) {
            // Send our input to the Host.
            std::string outData(reinterpret_cast<char*>(&localInput), sizeof(ClientInputPacket));
            bottomLayer.SendNetworkData(outData);

            // Overwrite local world with Host's World State.
            while (bottomLayer.HasIncomingData()) {
                std::string msg = bottomLayer.GetNextNetworkMessage();
                size_t offset = 0;

                while (offset < msg.size()) {
                    PacketType type = static_cast<PacketType>(msg[offset]);
                    if (type == PacketType::GAME_STATE && offset + sizeof(GameStatePacket) <= msg.size()) {
                        std::memcpy(&authoritativeState, msg.data() + offset, sizeof(GameStatePacket));
                        offset += sizeof(GameStatePacket);
                    }
                    else if (type == PacketType::PLAYER_ASSIGNMENT && offset + sizeof(PlayerAssignmentPacket) <= msg.size()) {
                        // Normally consumed during ConnectToGame; tolerate it if TCP delivery batches it later.
                        PlayerAssignmentPacket assignment{};
                        std::memcpy(&assignment, msg.data() + offset, sizeof(PlayerAssignmentPacket));
                        myLocalPlayerId = assignment.playerId;
                        offset += sizeof(PlayerAssignmentPacket);
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
        std::string overlay;
        if (bottomLayer.IsOutboundDelayEnabled()) {
            overlay = "F1 outbound delay ON: random " +
                std::to_string(bottomLayer.GetOutboundDelayMinMs()) + "-" +
                std::to_string(bottomLayer.GetOutboundDelayMaxMs()) + " ms";
        }
        else {
            overlay = "F1: toggle outbound delay test";
        }

        TopLayer::DrawGame(authoritativeState, myLocalPlayerId, overlay);
    }

    CloseWindow();
    return 0;
}

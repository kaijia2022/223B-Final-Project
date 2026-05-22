#include "raylib.h"
#include "ConnectForm.h"
#include "Engine.h"
#include "TopLayer.h"
#include "BottomLayer.h"
#include <string>
#include <cmath>

// Simple collision helper: Distance between player center and coin center
bool CheckPlayerCoinCollision(float px, float py, float cx, float cy) {
    float distanceSq = (px - cx) * (px - cx) + (py - cy) * (py - cy);
    // Player radius (~15px) + Coin radius (10px) = 25px. 25 * 25 = 625
    return distanceSq < 625.0f;
}

int main() {
    // Upgraded to 800x600 so the TopLayer HUD and Arena fit perfectly
    InitWindow(800, 600, "Gold Rush - Multiplayer Ready");
    SetTargetFPS(60);

    std::string targetIp = "";
    NetworkRole role = NetworkRole::NONE;

    // 1. Show the UI and block until the user makes a choice
    RunConnectionScreen(targetIp, role);

    if (role == NetworkRole::NONE) {
        CloseWindow();
        return 0;
    }

    // --- MULTIPLAYER SETUP (Commented out for single-player testing) ---
    // uint32_t myLocalPlayerId = (role == NetworkRole::HOST) ? 0 : 1; 
    // if (role == NetworkRole::HOST) bottomLayer.HostGame(8080);
    // else bottomLayer.ConnectToGame(targetIp, 8080);

    uint32_t myLocalPlayerId = 0; // Hardcoded local ID for single player setup

    // 2. Initialize World State Structure (Middle Layer Data)
    GameStatePacket authoritativeState = {};
    authoritativeState.frameNumber = 0;

    // Setup Local Player (Slot 0)
    authoritativeState.players[0].id = 0;
    authoritativeState.players[0].x = 400.0f; // Center of arena
    authoritativeState.players[0].y = 250.0f;
    authoritativeState.players[0].score = 0;
    authoritativeState.players[0].colorId = 0; // Red
    authoritativeState.players[0].active = true;

    // Explicitly deactivate other player slots for now
    for (int i = 1; i < MAX_PLAYERS; i++) {
        authoritativeState.players[i].active = false;
    }

    // Spawn 5 initial coins inside the arena boundaries (50 to 750 X, 50 to 500 Y)
    float mockCoinPositionsX[5] = { 150, 300, 600, 200, 550 };
    float mockCoinPositionsY[5] = { 120, 400, 200, 350, 420 };
    for (int i = 0; i < MAX_COINS; i++) {
        if (i < 5) {
            authoritativeState.coins[i].id = i;
            authoritativeState.coins[i].x = mockCoinPositionsX[i];
            authoritativeState.coins[i].y = mockCoinPositionsY[i];
            authoritativeState.coins[i].active = true;
        }
        else {
            authoritativeState.coins[i].active = false;
        }
    }

    // 3. Main Game Loop
    while (!WindowShouldClose()) {
        authoritativeState.frameNumber++;

        // ==========================================================
        // STEP A: INPUT GATHERING (Bottom Layer)
        // ==========================================================
        ClientInputPacket localInput = {};
        localInput.playerId = myLocalPlayerId;

        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) localInput.moveX = 1.0f;
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))  localInput.moveX = -1.0f;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))  localInput.moveY = 1.0f;
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))    localInput.moveY = -1.0f;

        // [FUTURE NETWORK HOOK - CLIENT]: 
        // If (role == CLIENT), send(localInput) to server and skip STEP B.
        // Instead, read incoming data via recv() to overwrite authoritativeState.

        // ==========================================================
        // STEP B: SIMULATION ENGINE (Middle Layer)
        // ==========================================================
        // [FUTURE NETWORK HOOK - HOST]:
        // If (role == HOST), loop through all connected player inputs before running this.

        float moveSpeed = 4.0f;

        // Update local player position based on packed input
        PlayerState& p = authoritativeState.players[0];
        p.x += localInput.moveX * moveSpeed;
        p.y += localInput.moveY * moveSpeed;

        // Keep player inside Arena Boundaries (Arena is 50x50 to 750x500)
        // Player square is 30x30, so half-extents are 15px
        if (p.x < 65.0f)  p.x = 65.0f;
        if (p.x > 735.0f) p.x = 735.0f;
        if (p.y < 65.0f)  p.y = 65.0f;
        if (p.y > 485.0f) p.y = 485.0f;

        // Check authoritative coin collection
        for (int i = 0; i < MAX_COINS; i++) {
            if (authoritativeState.coins[i].active) {
                if (CheckPlayerCoinCollision(p.x, p.y, authoritativeState.coins[i].x, authoritativeState.coins[i].y)) {

                    // Award point authoritatively
                    p.score += 10;

                    // Inactive the coin
                    authoritativeState.coins[i].active = false;

                    // OPTIONAL: Respawn coin somewhere else instantly
                    authoritativeState.coins[i].x = (float)GetRandomValue(100, 700);
                    authoritativeState.coins[i].y = (float)GetRandomValue(100, 450);
                    authoritativeState.coins[i].active = true;
                }
            }
        }

        // [FUTURE NETWORK HOOK - HOST]:
        // Broadcast the updated authoritativeState packet to all connected clients.

        // ==========================================================
        // STEP C: RENDER (Top Layer)
        // ==========================================================
        TopLayer::DrawGame(authoritativeState, myLocalPlayerId);
    }

    CloseWindow();
    return 0;
}
#pragma once
#include "raylib.h"
#include "Engine.h"
#include <string>

class TopLayer {
public:
    // Helper to map network IDs to Raylib colors cleanly
    static Color GetColorFromId(uint8_t colorId) {
        switch (colorId) {
        case 0: return RED;
        case 1: return BLUE;
        case 2: return LIME;
        case 3: return ORANGE;
        default: return GRAY;
        }
    }

    static void DrawGame(const GameStatePacket& state, uint32_t localPlayerId) {
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // 1. Draw Arena Boundary (e.g., 800x600 window, game area is 700x450)
        int arenaX = 50, arenaY = 50, arenaWidth = 700, arenaHeight = 450;
        DrawRectangleLines(arenaX, arenaY, arenaWidth, arenaHeight, DARKGRAY);

        // 2. Draw Golden Coins (Yellow Circles)
        for (int i = 0; i < MAX_COINS; i++) {
            if (state.coins[i].active) {
                // Draw coin body
                DrawCircle((int)state.coins[i].x, (int)state.coins[i].y, 10, YELLOW);
                // Draw coin outline
                DrawCircleLines((int)state.coins[i].x, (int)state.coins[i].y, 10, GOLD);
            }
        }

        // 3. Draw Players (Colored Squares)
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (state.players[i].active) {
                Color pColor = GetColorFromId(state.players[i].colorId);
                int size = 30;

                // Draw player filled square centered on coordinates
                DrawRectangle((int)state.players[i].x - size / 2, (int)state.players[i].y - size / 2, size, size, pColor);
                DrawRectangleLines((int)state.players[i].x - size / 2, (int)state.players[i].y - size / 2, size, size, BLACK);

                // Highlight the local player so the user knows who they are controlling
                if (state.players[i].id == localPlayerId) {
                    DrawRectangleLines((int)state.players[i].x - (size / 2 + 4), (int)state.players[i].y - (size / 2 + 4), size + 8, size + 8, PURPLE);
                }
            }
        }

        // 4. Draw Scoreboard UI (Bottom HUD)
        DrawText("SCOREBOARD:", 50, 520, 18, DARKGRAY);
        int uiOffset = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (state.players[i].active) {
                std::string scoreText = "P" + std::to_string(state.players[i].id) + ": " + std::to_string(state.players[i].score);
                Color pColor = GetColorFromId(state.players[i].colorId);

                DrawText(scoreText.c_str(), 200 + uiOffset, 520, 18, pColor);
                uiOffset += 120; // Space out names horizontally
            }
        }

        EndDrawing();
    }
};
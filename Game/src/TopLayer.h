#pragma once
#include "raylib.h"
#include "Engine.h"
#include <string>

class TopLayer {
public:
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

        const int arenaX = 50;
        const int arenaY = 50;
        const int arenaWidth = 700;
        const int arenaHeight = 450;
        DrawRectangleLines(arenaX, arenaY, arenaWidth, arenaHeight, DARKGRAY);

        for (int i = 0; i < MAX_COINS; i++) {
            if (state.coins[i].active) {
                DrawCircle((int)state.coins[i].x, (int)state.coins[i].y, 10, YELLOW);
                DrawCircleLines((int)state.coins[i].x, (int)state.coins[i].y, 10, GOLD);
            }
        }

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!state.players[i].active) continue;

            const Color pColor = GetColorFromId(state.players[i].colorId);
            const int size = 30;
            const int x = (int)state.players[i].x - size / 2;
            const int y = (int)state.players[i].y - size / 2;

            DrawRectangle(x, y, size, size, pColor);
            DrawRectangleLines(x, y, size, size, BLACK);

            std::string label = "P" + std::to_string(state.players[i].id);
            DrawText(label.c_str(), x + 5, y + 6, 16, WHITE);

            if (state.players[i].id == localPlayerId) {
                DrawRectangleLines(x - 4, y - 4, size + 8, size + 8, PURPLE);
            }
        }

        DrawText("SCOREBOARD:", 50, 520, 18, DARKGRAY);
        int uiOffset = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!state.players[i].active) continue;

            std::string scoreText = "P" + std::to_string(state.players[i].id) + ": " + std::to_string(state.players[i].score);
            Color pColor = GetColorFromId(state.players[i].colorId);
            DrawText(scoreText.c_str(), 200 + uiOffset, 520, 18, pColor);
            uiOffset += 120;
        }

        std::string frameText = "Frame: " + std::to_string(state.frameNumber) + " | You are P" + std::to_string(localPlayerId);
        DrawText(frameText.c_str(), 50, 555, 16, DARKGRAY);
        DrawText("Move with WASD or Arrow Keys", 420, 555, 16, DARKGRAY);

        EndDrawing();
    }
};

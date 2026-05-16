#include "ConnectForm.h"
#include "raylib.h"
#include <string>

void RunConnectionScreen(std::string& outIpAddress, NetworkRole& outRole) {
    std::string inputText = "";
    int maxIpLength = 15; // e.g., "255.255.255.255"

    Rectangle textBox = { 250, 200, 300, 40 };
    Rectangle hostButton = { 250, 260, 140, 40 };
    Rectangle joinButton = { 410, 260, 140, 40 };

    while (!WindowShouldClose() && outRole == NetworkRole::NONE) {

        // 1. Handle Text Input for IP Address
        int key = GetCharPressed();
        while (key > 0) {
            if (((key >= '0' && key <= '9') || key == '.') && (inputText.length() < maxIpLength)) {
                inputText += (char)key;
            }
            key = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE) && inputText.length() > 0) {
            inputText.pop_back();
        }

        // 2. Handle Mouse Clicks for Buttons
        Vector2 mousePoint = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointRec(mousePoint, hostButton)) {
                outRole = NetworkRole::HOST;
                // Host doesn't need a target IP, it just opens a port
            }
            else if (CheckCollisionPointRec(mousePoint, joinButton)) {
                outRole = NetworkRole::CLIENT;
                // IF text is empty, default to Localhost!
                if (inputText.empty()) {
                    outIpAddress = "127.0.0.1";
                }
                else {
                    outIpAddress = inputText;
                }
            }
        }

        // 3. Draw the UI
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawText("MULTIPLAYER SETUP", 250, 150, 20, DARKGRAY);

        // Draw Text Box
        DrawRectangleRec(textBox, LIGHTGRAY);
        DrawRectangleLines((int)textBox.x, (int)textBox.y, (int)textBox.width, (int)textBox.height, DARKGRAY);

        if (inputText.empty()) {
            // Show a helpful placeholder if empty
            DrawText("Leave blank for Localhost", (int)textBox.x + 5, (int)textBox.y + 10, 20, GRAY);
        }
        else {
            DrawText(inputText.c_str(), (int)textBox.x + 5, (int)textBox.y + 10, 20, MAROON);
        }

        // Draw blinking cursor
        // GetTime() * 2 makes it blink twice a second. Casting to int lets us use modulo.
        if ((int)(GetTime() * 2) % 2 == 0 && inputText.length() < maxIpLength) {
            int textWidth = inputText.empty() ? 0 : MeasureText(inputText.c_str(), 20);
            DrawText("_", (int)textBox.x + 8 + textWidth, (int)textBox.y + 10, 20, MAROON);
        }

        // Draw Host Button
        DrawRectangleRec(hostButton, CheckCollisionPointRec(mousePoint, hostButton) ? GRAY : DARKGRAY);
        DrawText("HOST GAME", (int)hostButton.x + 15, (int)hostButton.y + 10, 20, RAYWHITE);

        // Draw Join Button
        DrawRectangleRec(joinButton, CheckCollisionPointRec(mousePoint, joinButton) ? GRAY : DARKGRAY);

        // Dynamically change the button text!
        if (inputText.empty()) {
            DrawText("JOIN LOCAL", (int)joinButton.x + 15, (int)joinButton.y + 10, 20, RAYWHITE);
        }
        else {
            DrawText("JOIN IP", (int)joinButton.x + 35, (int)joinButton.y + 10, 20, RAYWHITE);
        }

        EndDrawing();
    }
}
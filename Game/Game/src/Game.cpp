#include "raylib.h"
#include "ConnectForm.h"

int main() {
    InitWindow(800, 450, "Hamachi Test Client");
    SetTargetFPS(60);

    std::string targetIp = "";
    NetworkRole role = NetworkRole::NONE;

    // 1. Show the UI and block until the user makes a choice
    RunConnectionScreen(targetIp, role);

    // 2. Apply the choice to your BottomLayer
    if (role == NetworkRole::HOST) {
        // bottomLayer.HostGame(8080);
    }
    else if (role == NetworkRole::CLIENT) {
        // bottomLayer.ConnectToGame(targetIp, 8080);
    }
    else {
        // User closed the window before choosing
        CloseWindow();
        return 0;
    }

    // 3. Proceed to the main game loop!
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("Game is running! Check console for network status.", 100, 200, 20, DARKGRAY);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#include "raylib.h"
#include "ConnectForm.h"
#include "Engine.h"
#include "TopLayer.h"
#include "BottomLayer.h"
#include <string>


ClientInputPacket BuildLocalInput(uint32_t myLocalPlayerId, uint32_t frame) {
    ClientInputPacket localInput = {};
    localInput.type = PacketType::CLIENT_INPUT;
    localInput.playerId = myLocalPlayerId;
    localInput.frameNumber = frame;

    // Actions only: send direction intent, not world-space coordinates.
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) localInput.moveX += 1.0f;
    if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) localInput.moveX -= 1.0f;
    if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) localInput.moveY += 1.0f;
    if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) localInput.moveY -= 1.0f;

    return localInput;
}

void DrawCenteredStatus(const char* title, const std::string& line1, const std::string& line2) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText(title, 140, 170, 24, DARKGRAY);
    DrawText(line1.c_str(), 140, 225, 20, MAROON);
    DrawText(line2.c_str(), 140, 260, 20, GRAY);
    EndDrawing();
}

void ProcessGameplayPacket(const std::string& msg, uint32_t myLocalPlayerId, uint32_t currentFrame) {
    if (msg.empty()) return;

    const PacketType type = PeekPacketType(msg);
    if (type == PacketType::CLIENT_INPUT) {
        ClientInputPacket remoteInput{};
        if (DeserializeInput(msg, remoteInput) && remoteInput.playerId != myLocalPlayerId) {
            HandleRemoteInput(remoteInput, currentFrame);
        }
    }
}


int main() {
    InitWindow(800, 600, "Gold Rush - P2P Rollback Multiplayer Test");
    SetTargetFPS(60);

    std::string targetIp;
    NetworkRole role = NetworkRole::NONE;
    RunConnectionScreen(targetIp, role);

    if (role == NetworkRole::NONE) {
        CloseWindow();
        return 0;
    }

    BottomLayer bottomLayer;
    if (role == NetworkRole::HOST) {
        if (!bottomLayer.HostGame(8080)) {
            CloseWindow();
            return 1;
        }
    } else {
        if (!bottomLayer.ConnectToGame(targetIp, 8080)) {
            CloseWindow();
            return 1;
        }
    }

    if (role == NetworkRole::HOST) {
        while (!WindowShouldClose() && !IsKeyPressed(KEY_ENTER)) {
            DrawCenteredStatus(
                "HOST LOBBY",
                "Clients may join. Press ENTER to start. Host is discovery only; gameplay is P2P.",
                "Direct peers connected: " + std::to_string(bottomLayer.GetConnectedPeerCount())
            );
        }
        bottomLayer.BroadcastStartGame();
    } else {
        while (!WindowShouldClose() && (!bottomLayer.HasPlayerAssignment() || !bottomLayer.IsGameStarted())) {
            std::string line1 = bottomLayer.HasPlayerAssignment()
                ? "Assigned player " + std::to_string(bottomLayer.GetLocalPlayerId()) + ". Waiting for host start."
                : "Waiting for player assignment from host.";

            DrawCenteredStatus(
                "CLIENT LOBBY",
                line1,
                "Local peer port: " + std::to_string(bottomLayer.GetLocalPeerPort()) +
                    " | Direct peers: " + std::to_string(bottomLayer.GetConnectedPeerCount())
            );
        }
    }

    if (WindowShouldClose()) {
        CloseWindow();
        return 0;
    }

    const uint32_t myLocalPlayerId = bottomLayer.GetLocalPlayerId();
    const uint32_t activePlayerMask = bottomLayer.GetKnownPlayerMask();

    GameStatePacket currentState = {};
    InitializeGameState(currentState, activePlayerMask);
    ResetRollbackBuffers();
    stateHistory[0] = currentState;

    while (!WindowShouldClose()) {
        while (bottomLayer.HasIncomingData()) {
            std::string msg = bottomLayer.GetNextNetworkMessage();
            ProcessGameplayPacket(msg, myLocalPlayerId, currentState.frameNumber);
        }

        if (needsRollback) {
            RollbackAndReplay(rollbackFrame, currentState.frameNumber, currentState);
            needsRollback = false;
        }

        uint32_t missingPlayerId = UINT32_MAX;
        uint32_t missingFrame = 0;
        const bool freezeForInput = ShouldFreezeForMissingInput(
            currentState,
            myLocalPlayerId,
            currentState.frameNumber,
            &missingPlayerId,
            &missingFrame
        );

        if (!freezeForInput) {
            currentState.frameNumber++;
            const uint32_t frame = currentState.frameNumber;
            const int index = static_cast<int>(frame % INPUT_BUFFER_SIZE);

            ClientInputPacket localInput = BuildLocalInput(myLocalPlayerId, frame);
            StoreLocalInput(localInput);
            bottomLayer.SendNetworkData(SerializeInput(localInput));

            ClientInputPacket frameInputs[MAX_PLAYERS] = {};
            BuildFrameInputs(currentState, frame, frameInputs);

            stateHistory[index] = currentState;
            SimulateFrame(currentState, frameInputs);
        } else {
            // Keep resending this client's confirmed local actions inside the
            // rollback window. This helps a peer catch up if the direct P2P
            // connection was established slightly late.
            const uint32_t firstFrame = currentState.frameNumber > MAX_ROLLBACK_FRAMES
                ? currentState.frameNumber - MAX_ROLLBACK_FRAMES
                : 1;
            for (uint32_t frame = firstFrame; frame <= currentState.frameNumber; ++frame) {
                const int index = static_cast<int>(frame % INPUT_BUFFER_SIZE);
                if (myLocalPlayerId < MAX_PLAYERS &&
                    hasInput[myLocalPlayerId][index] &&
                    !wasPredicted[myLocalPlayerId][index] &&
                    inputHistory[myLocalPlayerId][index].frameNumber == frame) {
                    bottomLayer.SendNetworkData(SerializeInput(inputHistory[myLocalPlayerId][index]));
                }
            }
        }

        std::string overlay;
        if (freezeForInput) {
            overlay = "Rollback window full: waiting for P" + std::to_string(missingPlayerId) +
                " input for frame " + std::to_string(missingFrame) + ".";
        }

        TopLayer::DrawGame(currentState, myLocalPlayerId, overlay);
    }

    CloseWindow();
    return 0;
}

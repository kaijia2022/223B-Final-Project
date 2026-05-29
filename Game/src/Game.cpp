#include "raylib.h"
#include "ConnectForm.h"
#include "Engine.h"
#include "TopLayer.h"
#include "BottomLayer.h"
#include <string>
#include <limits>

namespace {
ClientInputPacket BuildLocalInput(
    uint32_t myLocalPlayerId,
    uint32_t frame,
    const GameStatePacket& currentState
) {
    ClientInputPacket localInput = {};
    localInput.type = PacketType::CLIENT_INPUT;
    localInput.playerId = myLocalPlayerId;
    localInput.frameNumber = frame;

    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) localInput.moveX += 1.0f;
    if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) localInput.moveX -= 1.0f;
    if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) localInput.moveY += 1.0f;
    if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) localInput.moveY -= 1.0f;

    // Include our latest known player state. Receivers use input history +
    // rollback when possible, and use this player-state hint when the packet is
    // outside their rollback window.
    if (myLocalPlayerId < MAX_PLAYERS) {
        localInput.playerState = currentState.players[myLocalPlayerId];
    }

    return localInput;
}

void DrawCenteredStatus(const char* title, const char* line1, const char* line2) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText(title, 190, 190, 24, DARKGRAY);
    DrawText(line1, 190, 240, 20, MAROON);
    DrawText(line2, 190, 275, 20, GRAY);
    EndDrawing();
}

void ProcessNetworkMessage(
    const std::string& msg,
    uint32_t myLocalPlayerId,
    uint32_t currentFrame,
    GameStatePacket& currentState,
    bool& hasStartSignal,
    bool& hasAssignment,
    uint32_t& assignedPlayerId
) {
    if (msg.empty()) return;

    const PacketType type = PeekPacketType(msg);

    if (type == PacketType::ASSIGN_PLAYER) {
        AssignPlayerPacket assign{};
        if (DeserializeAssignPlayer(msg, assign)) {
            assignedPlayerId = assign.playerId;
            hasAssignment = true;
        }
        return;
    }

    if (type == PacketType::START_GAME) {
        StartGamePacket start{};
        if (DeserializeStartGame(msg, start)) {
            hasStartSignal = true;
        }
        return;
    }

    if (type == PacketType::CLIENT_INPUT) {
        ClientInputPacket remoteInput{};
        if (DeserializeInput(msg, remoteInput)) {
            // Local input is already stored immediately. Ignore our echo from the host relay.
            if (remoteInput.playerId != myLocalPlayerId) {
                HandleRemoteInput(remoteInput, currentFrame, currentState);
            }
        }
    }
}
}

int main() {
    InitWindow(800, 600, "Gold Rush - Rollback Multiplayer Test");
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

    uint32_t myLocalPlayerId = (role == NetworkRole::HOST)
        ? 0u
        : std::numeric_limits<uint32_t>::max();

    bool hasAssignment = (role == NetworkRole::HOST);
    bool hasStartSignal = (role == NetworkRole::HOST);
    GameStatePacket lobbyState = {};

    if (role == NetworkRole::HOST) {
        while (!WindowShouldClose() && !IsKeyPressed(KEY_ENTER)) {
            while (bottomLayer.HasIncomingData()) {
                // Discard CONNECT packets; assignment is handled by BottomLayer on accept.
                (void)bottomLayer.GetNextNetworkMessage();
            }
            DrawCenteredStatus(
                "HOST LOBBY",
                "Clients may join now. Press ENTER to start.",
                "Controls: WASD / Arrow Keys. Max players: 4."
            );
        }

        StartGamePacket start{};
        bottomLayer.SendNetworkData(SerializeStartGame(start));
    } else {
        ReadyToStartPacket hello{};
        bottomLayer.SendNetworkData(SerializePacket(hello));

        while (!WindowShouldClose() && (!hasAssignment || !hasStartSignal)) {
            while (bottomLayer.HasIncomingData()) {
                std::string msg = bottomLayer.GetNextNetworkMessage();
                ProcessNetworkMessage(
                    msg,
                    myLocalPlayerId,
                    0,
                    lobbyState,
                    hasStartSignal,
                    hasAssignment,
                    myLocalPlayerId
                );
            }

            DrawCenteredStatus(
                "CLIENT LOBBY",
                hasAssignment ? "Assigned player ID. Waiting for host start." : "Waiting for player assignment.",
                "Ask the host to press ENTER after everyone joins."
            );
        }
    }

    if (WindowShouldClose()) {
        CloseWindow();
        return 0;
    }

    GameStatePacket currentState = {};
    InitializeGameState(currentState);
    ResetRollbackBuffers();

    // Save frame 0 for rollback safety.
    stateHistory[0] = currentState;

    while (!WindowShouldClose()) {
        while (bottomLayer.HasIncomingData()) {
            std::string msg = bottomLayer.GetNextNetworkMessage();
            ProcessNetworkMessage(
                msg,
                myLocalPlayerId,
                currentState.frameNumber,
                currentState,
                hasStartSignal,
                hasAssignment,
                myLocalPlayerId
            );
        }

        if (needsRollback) {
            RollbackAndReplay(rollbackFrame, currentState.frameNumber, currentState);
            needsRollback = false;
        }

        currentState.frameNumber++;
        const uint32_t frame = currentState.frameNumber;
        const int index = static_cast<int>(frame % INPUT_BUFFER_SIZE);

        ClientInputPacket localInput = BuildLocalInput(myLocalPlayerId, frame, currentState);
        StoreLocalInput(localInput);
        bottomLayer.SendNetworkData(SerializeInput(localInput));

        ClientInputPacket frameInputs[MAX_PLAYERS] = {};
        BuildFrameInputs(currentState, frame, frameInputs);

        stateHistory[index] = currentState;
        SimulateFrame(currentState, frameInputs);

        TopLayer::DrawGame(currentState, myLocalPlayerId);
    }

    CloseWindow();
    return 0;
}

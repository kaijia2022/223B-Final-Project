#include "Engine.h"
#include <cstring>

ClientInputPacket inputHistory[MAX_PLAYERS][INPUT_BUFFER_SIZE] = {};
bool hasInput[MAX_PLAYERS][INPUT_BUFFER_SIZE] = {};
bool wasPredicted[MAX_PLAYERS][INPUT_BUFFER_SIZE] = {};
GameStatePacket stateHistory[INPUT_BUFFER_SIZE] = {};
bool needsRollback = false;
uint32_t rollbackFrame = 0;

bool CheckPlayerCoinCollision(float px, float py, float cx, float cy) {
    const float distanceSq = (px - cx) * (px - cx) + (py - cy) * (py - cy);
    return distanceSq < 625.0f;
}

void SetActivePlayers(GameStatePacket& state, uint32_t activePlayerMask) {
    for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
        state.players[i].id = i;
        state.players[i].colorId = static_cast<uint8_t>(i);
        state.players[i].active = ((activePlayerMask & (1u << i)) != 0);
    }
}

void InitializeGameState(GameStatePacket& state, uint32_t activePlayerMask) {
    std::memset(&state, 0, sizeof(GameStatePacket));
    state.type = PacketType::GAME_STATE_RESYNC;
    state.frameNumber = 0;

    const float startX[MAX_PLAYERS] = { 200.0f, 600.0f, 200.0f, 600.0f };
    const float startY[MAX_PLAYERS] = { 160.0f, 160.0f, 400.0f, 400.0f };

    for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
        state.players[i].id = i;
        state.players[i].x = startX[i];
        state.players[i].y = startY[i];
        state.players[i].score = 0;
        state.players[i].colorId = static_cast<uint8_t>(i);
        state.players[i].active = ((activePlayerMask & (1u << i)) != 0);
    }

    const float coinX[5] = { 150, 300, 600, 200, 550 };
    const float coinY[5] = { 120, 400, 200, 350, 420 };

    for (uint32_t i = 0; i < MAX_COINS; ++i) {
        state.coins[i].id = i;
        if (i < 5) {
            state.coins[i].x = coinX[i];
            state.coins[i].y = coinY[i];
            state.coins[i].active = true;
        } else {
            state.coins[i].active = false;
        }
    }
}

void ResetRollbackBuffers() {
    std::memset(inputHistory, 0, sizeof(inputHistory));
    std::memset(hasInput, 0, sizeof(hasInput));
    std::memset(wasPredicted, 0, sizeof(wasPredicted));
    std::memset(stateHistory, 0, sizeof(stateHistory));
    needsRollback = false;
    rollbackFrame = 0;
}

void SimulateFrame(GameStatePacket& state, const ClientInputPacket inputs[MAX_PLAYERS]) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!state.players[i].active) continue;

        PlayerState& p = state.players[i];
        const ClientInputPacket& input = inputs[i];

        p.x += input.moveX * MOVE_SPEED;
        p.y += input.moveY * MOVE_SPEED;

        if (p.x < 65.0f)  p.x = 65.0f;
        if (p.x > 735.0f) p.x = 735.0f;
        if (p.y < 65.0f)  p.y = 65.0f;
        if (p.y > 485.0f) p.y = 485.0f;

        for (int c = 0; c < MAX_COINS; ++c) {
            if (!state.coins[c].active) continue;
            if (CheckPlayerCoinCollision(p.x, p.y, state.coins[c].x, state.coins[c].y)) {
                p.score += 10;
                state.coins[c].active = false;
            }
        }
    }
}

bool SameInput(const ClientInputPacket& a, const ClientInputPacket& b) {
    return a.playerId == b.playerId &&
           a.frameNumber == b.frameNumber &&
           a.moveX == b.moveX &&
           a.moveY == b.moveY;
}

bool IsConfirmedInput(uint32_t playerId, uint32_t frameNumber) {
    if (playerId >= MAX_PLAYERS) return false;
    const int index = static_cast<int>(frameNumber % INPUT_BUFFER_SIZE);
    return hasInput[playerId][index] &&
           !wasPredicted[playerId][index] &&
           inputHistory[playerId][index].frameNumber == frameNumber &&
           inputHistory[playerId][index].playerId == playerId;
}

ClientInputPacket PredictInput(uint32_t playerId, uint32_t frameNumber) {
    ClientInputPacket predicted = {};
    predicted.type = PacketType::CLIENT_INPUT;
    predicted.playerId = playerId;
    predicted.frameNumber = frameNumber;

    if (frameNumber > 0) {
        const int previousIndex = static_cast<int>((frameNumber - 1) % INPUT_BUFFER_SIZE);
        const ClientInputPacket& previous = inputHistory[playerId][previousIndex];
        if (previous.playerId == playerId && previous.frameNumber == frameNumber - 1) {
            predicted.moveX = previous.moveX;
            predicted.moveY = previous.moveY;
        }
    }

    return predicted;
}

void StoreLocalInput(const ClientInputPacket& input) {
    if (input.playerId >= MAX_PLAYERS) return;
    const int index = static_cast<int>(input.frameNumber % INPUT_BUFFER_SIZE);
    inputHistory[input.playerId][index] = input;
    hasInput[input.playerId][index] = true;
    wasPredicted[input.playerId][index] = false;
}

void HandleRemoteInput(const ClientInputPacket& remoteInput, uint32_t currentFrame) {
    if (remoteInput.playerId >= MAX_PLAYERS) return;

    // Far-future packets are kept only when their ring slot is safe. In normal
    // operation the 50-frame stall rule prevents us from running too far away
    // from unconfirmed peer inputs.
    if (remoteInput.frameNumber >= currentFrame + INPUT_BUFFER_SIZE) return;
    if (remoteInput.frameNumber + INPUT_BUFFER_SIZE <= currentFrame) return;

    const int index = static_cast<int>(remoteInput.frameNumber % INPUT_BUFFER_SIZE);
    const ClientInputPacket oldInput = inputHistory[remoteInput.playerId][index];
    const bool oldWasPrediction =
        hasInput[remoteInput.playerId][index] &&
        wasPredicted[remoteInput.playerId][index] &&
        oldInput.frameNumber == remoteInput.frameNumber;

    const bool predictionWasWrong = oldWasPrediction && !SameInput(oldInput, remoteInput);

    inputHistory[remoteInput.playerId][index] = remoteInput;
    hasInput[remoteInput.playerId][index] = true;
    wasPredicted[remoteInput.playerId][index] = false;

    const bool canRollbackToFrame =
        remoteInput.frameNumber <= currentFrame &&
        remoteInput.frameNumber + MAX_ROLLBACK_FRAMES >= currentFrame;

    if (canRollbackToFrame && predictionWasWrong) {
        if (!needsRollback || remoteInput.frameNumber < rollbackFrame) {
            needsRollback = true;
            rollbackFrame = remoteInput.frameNumber;
        }
    }
}

void BuildFrameInputs(const GameStatePacket& state, uint32_t frame, ClientInputPacket outInputs[MAX_PLAYERS]) {
    const int index = static_cast<int>(frame % INPUT_BUFFER_SIZE);

    for (uint32_t playerId = 0; playerId < MAX_PLAYERS; ++playerId) {
        if (!state.players[playerId].active) {
            outInputs[playerId] = {};
            continue;
        }

        if (hasInput[playerId][index] && inputHistory[playerId][index].frameNumber == frame) {
            outInputs[playerId] = inputHistory[playerId][index];
        } else {
            ClientInputPacket predicted = PredictInput(playerId, frame);
            inputHistory[playerId][index] = predicted;
            hasInput[playerId][index] = true;
            wasPredicted[playerId][index] = true;
            outInputs[playerId] = predicted;
        }
    }
}

bool ShouldFreezeForMissingInput(const GameStatePacket& state, uint32_t myLocalPlayerId, uint32_t currentFrame, uint32_t* missingPlayerId, uint32_t* missingFrame) {
    if (currentFrame <= MAX_ROLLBACK_FRAMES) return false;

    const uint32_t requiredFrame = currentFrame - MAX_ROLLBACK_FRAMES;
    for (uint32_t playerId = 0; playerId < MAX_PLAYERS; ++playerId) {
        if (playerId == myLocalPlayerId) continue;
        if (!state.players[playerId].active) continue;

        if (!IsConfirmedInput(playerId, requiredFrame)) {
            if (missingPlayerId) *missingPlayerId = playerId;
            if (missingFrame) *missingFrame = requiredFrame;
            return true;
        }
    }

    return false;
}

void RollbackAndReplay(uint32_t rollbackStartFrame, uint32_t currentFrame, GameStatePacket& currentState) {
    if (rollbackStartFrame + MAX_ROLLBACK_FRAMES < currentFrame) return;
    if (rollbackStartFrame >= currentFrame) return;

    currentState = stateHistory[rollbackStartFrame % INPUT_BUFFER_SIZE];

    for (uint32_t frame = rollbackStartFrame; frame <= currentFrame; ++frame) {
        currentState.frameNumber = frame;
        ClientInputPacket frameInputs[MAX_PLAYERS] = {};
        BuildFrameInputs(currentState, frame, frameInputs);
        stateHistory[frame % INPUT_BUFFER_SIZE] = currentState;
        SimulateFrame(currentState, frameInputs);
    }

    currentState.frameNumber = currentFrame;
}

PacketType PeekPacketType(const std::string& payload) {
    if (payload.empty()) return PacketType::JOIN_REQUEST;
    return static_cast<PacketType>(static_cast<uint8_t>(payload[0]));
}

std::string SerializeInput(const ClientInputPacket& packet) { return SerializePacket(packet); }
std::string SerializeJoinRequest(const JoinRequestPacket& packet) { return SerializePacket(packet); }
std::string SerializeAssignPlayer(const AssignPlayerPacket& packet) { return SerializePacket(packet); }
std::string SerializePeerList(const PeerListPacket& packet) { return SerializePacket(packet); }
std::string SerializeStartGame(const StartGamePacket& packet) { return SerializePacket(packet); }
std::string SerializePeerHello(const PeerHelloPacket& packet) { return SerializePacket(packet); }

bool DeserializeInput(const std::string& payload, ClientInputPacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::CLIENT_INPUT); }
bool DeserializeJoinRequest(const std::string& payload, JoinRequestPacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::JOIN_REQUEST); }
bool DeserializeAssignPlayer(const std::string& payload, AssignPlayerPacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::ASSIGN_PLAYER); }
bool DeserializePeerList(const std::string& payload, PeerListPacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::PEER_LIST); }
bool DeserializeStartGame(const std::string& payload, StartGamePacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::START_GAME); }
bool DeserializePeerHello(const std::string& payload, PeerHelloPacket& outPacket) { return DeserializePacket(payload, outPacket, PacketType::PEER_HELLO); }

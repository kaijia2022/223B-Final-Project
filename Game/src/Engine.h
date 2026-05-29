#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstring>
#include <algorithm>

// Keep packets byte-stable for simple TCP payload serialization.
#pragma pack(push, 1)

enum class PacketType : uint8_t {
    CONNECT = 0,
    CLIENT_INPUT = 1,
    GAME_STATE = 2,
    ASSIGN_PLAYER = 3,
    START_GAME = 4
};

#define MAX_PLAYERS 4
#define MAX_COINS 15

struct PlayerState {
    uint32_t id;
    float x;
    float y;
    uint32_t score;
    uint8_t colorId;
    bool active;
};

struct CoinState {
    uint32_t id;
    float x;
    float y;
    bool active;
};

struct ReadyToStartPacket {
    PacketType type = PacketType::CONNECT;
};

struct StartGamePacket {
    PacketType type = PacketType::START_GAME;
};

struct AssignPlayerPacket {
    PacketType type = PacketType::ASSIGN_PLAYER;
    uint32_t playerId = 0;
};

struct GameStatePacket {
    PacketType type = PacketType::GAME_STATE;
    uint32_t frameNumber;
    PlayerState players[MAX_PLAYERS];
    CoinState coins[MAX_COINS];
};

struct ClientInputPacket {
    PacketType type = PacketType::CLIENT_INPUT;
    uint32_t playerId;
    uint32_t frameNumber;
    float moveX;
    float moveY;
};

#pragma pack(pop)

constexpr int INPUT_BUFFER_SIZE = 256;
constexpr int MAX_ROLLBACK_FRAMES = 8;
constexpr float MOVE_SPEED = 4.0f;

extern ClientInputPacket inputHistory[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern bool hasInput[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern bool wasPredicted[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern GameStatePacket stateHistory[INPUT_BUFFER_SIZE];
extern bool needsRollback;
extern uint32_t rollbackFrame;

bool CheckPlayerCoinCollision(float px, float py, float cx, float cy);
void InitializeGameState(GameStatePacket& state);
void ResetRollbackBuffers();

void SimulateFrame(GameStatePacket& state, const ClientInputPacket inputs[MAX_PLAYERS]);
bool SameInput(const ClientInputPacket& a, const ClientInputPacket& b);
ClientInputPacket PredictInput(uint32_t playerId, uint32_t frameNumber);
void StoreLocalInput(const ClientInputPacket& input);
void HandleRemoteInput(const ClientInputPacket& remoteInput, uint32_t currentFrame);
void BuildFrameInputs(const GameStatePacket& state, uint32_t frame, ClientInputPacket outInputs[MAX_PLAYERS]);
void RollbackAndReplay(uint32_t rollbackFrame, uint32_t currentFrame, GameStatePacket& currentState);

PacketType PeekPacketType(const std::string& payload);

std::string SerializeInput(const ClientInputPacket& packet);
std::string SerializeStartGame(const StartGamePacket& packet);
std::string SerializeAssignPlayer(const AssignPlayerPacket& packet);

bool DeserializeInput(const std::string& payload, ClientInputPacket& outPacket);
bool DeserializeAssignPlayer(const std::string& payload, AssignPlayerPacket& outPacket);
bool DeserializeStartGame(const std::string& payload, StartGamePacket& outPacket);

template <typename T>
std::string SerializePacket(const T& packet) {
    return std::string(reinterpret_cast<const char*>(&packet), sizeof(T));
}

template <typename T>
bool DeserializePacket(const std::string& payload, T& outPacket, PacketType expectedType) {
    if (payload.size() != sizeof(T)) return false;
    std::memcpy(&outPacket, payload.data(), sizeof(T));
    return outPacket.type == expectedType;
}

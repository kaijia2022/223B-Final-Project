#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#pragma pack(push, 1)

enum class PacketType : uint8_t {
    JOIN_REQUEST = 0,
    ASSIGN_PLAYER = 1,
    PEER_LIST = 2,
    START_GAME = 3,
    CLIENT_INPUT = 4,
    GAME_STATE_RESYNC = 5,
    PEER_HELLO = 6
};

#define MAX_PLAYERS 4
#define MAX_COINS 15
#define PEER_IP_STRING_SIZE 46

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

struct PeerInfo {
    uint32_t playerId;
    char ip[PEER_IP_STRING_SIZE];
    uint16_t port;
    bool active;
};

struct JoinRequestPacket {
    PacketType type = PacketType::JOIN_REQUEST;
    uint16_t peerPort = 0;
};

struct AssignPlayerPacket {
    PacketType type = PacketType::ASSIGN_PLAYER;
    uint32_t playerId = 0;
};

struct PeerListPacket {
    PacketType type = PacketType::PEER_LIST;
    uint32_t peerCount = 0;
    PeerInfo peers[MAX_PLAYERS];
};

struct StartGamePacket {
    PacketType type = PacketType::START_GAME;
};

struct PeerHelloPacket {
    PacketType type = PacketType::PEER_HELLO;
    uint32_t playerId = 0;
};

struct GameStatePacket {
    PacketType type = PacketType::GAME_STATE_RESYNC;
    uint32_t frameNumber = 0;
    PlayerState players[MAX_PLAYERS];
    CoinState coins[MAX_COINS];
};

// Gameplay packet: actions only. No world coordinates are sent.
struct ClientInputPacket {
    PacketType type = PacketType::CLIENT_INPUT;
    uint32_t playerId = 0;
    uint32_t frameNumber = 0;
    float moveX = 0.0f; // -1 left, +1 right
    float moveY = 0.0f; // -1 up, +1 down
};

#pragma pack(pop)

constexpr int INPUT_BUFFER_SIZE = 256;
constexpr int MAX_ROLLBACK_FRAMES = 50;
constexpr float MOVE_SPEED = 4.0f;

extern ClientInputPacket inputHistory[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern bool hasInput[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern bool wasPredicted[MAX_PLAYERS][INPUT_BUFFER_SIZE];
extern GameStatePacket stateHistory[INPUT_BUFFER_SIZE];
extern bool needsRollback;
extern uint32_t rollbackFrame;

bool CheckPlayerCoinCollision(float px, float py, float cx, float cy);
void InitializeGameState(GameStatePacket& state, uint32_t activePlayerMask = 0x0F);
void SetActivePlayers(GameStatePacket& state, uint32_t activePlayerMask);
void ResetRollbackBuffers();

void SimulateFrame(GameStatePacket& state, const ClientInputPacket inputs[MAX_PLAYERS]);
bool SameInput(const ClientInputPacket& a, const ClientInputPacket& b);
bool IsConfirmedInput(uint32_t playerId, uint32_t frameNumber);
ClientInputPacket PredictInput(uint32_t playerId, uint32_t frameNumber);
void StoreLocalInput(const ClientInputPacket& input);
void HandleRemoteInput(const ClientInputPacket& remoteInput, uint32_t currentFrame);
void BuildFrameInputs(const GameStatePacket& state, uint32_t frame, ClientInputPacket outInputs[MAX_PLAYERS]);
void RollbackAndReplay(uint32_t rollbackFrame, uint32_t currentFrame, GameStatePacket& currentState);
bool ShouldFreezeForMissingInput(const GameStatePacket& state, uint32_t myLocalPlayerId, uint32_t currentFrame, uint32_t* missingPlayerId = nullptr, uint32_t* missingFrame = nullptr);

PacketType PeekPacketType(const std::string& payload);

std::string SerializeInput(const ClientInputPacket& packet);
std::string SerializeJoinRequest(const JoinRequestPacket& packet);
std::string SerializeAssignPlayer(const AssignPlayerPacket& packet);
std::string SerializePeerList(const PeerListPacket& packet);
std::string SerializeStartGame(const StartGamePacket& packet);
std::string SerializePeerHello(const PeerHelloPacket& packet);

bool DeserializeInput(const std::string& payload, ClientInputPacket& outPacket);
bool DeserializeJoinRequest(const std::string& payload, JoinRequestPacket& outPacket);
bool DeserializeAssignPlayer(const std::string& payload, AssignPlayerPacket& outPacket);
bool DeserializePeerList(const std::string& payload, PeerListPacket& outPacket);
bool DeserializeStartGame(const std::string& payload, StartGamePacket& outPacket);
bool DeserializePeerHello(const std::string& payload, PeerHelloPacket& outPacket);

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

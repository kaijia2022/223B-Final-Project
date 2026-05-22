#pragma once
#include <cstdint>

// Enforce strict byte alignment for network safety
#pragma pack(push, 1)

enum class PacketType : uint8_t {
    CLIENT_INPUT = 1,
    GAME_STATE = 2
};

// Fixed limits keep memory footprint static and safe for raw casting
#define MAX_PLAYERS 4
#define MAX_COINS 15

// Data representing a single player
struct PlayerState {
    uint32_t id;
    float x;
    float y;
    uint32_t score;
    uint8_t colorId; // 0 = Red, 1 = Blue, 2 = Green, 3 = Orange (Map locally)
    bool active;
};

// Data representing a single coin
struct CoinState {
    uint32_t id;
    float x;
    float y;
    bool active;
};

// SERVER -> CLIENT: The absolute, authoritative state of the world
struct GameStatePacket {
    PacketType type = PacketType::GAME_STATE;
    uint32_t frameNumber; // Useful for tracking lockstep sync/tick rate
    PlayerState players[MAX_PLAYERS];
    CoinState coins[MAX_COINS];
};

// CLIENT -> SERVER: The client only requests movement intent
struct ClientInputPacket {
    PacketType type = PacketType::CLIENT_INPUT;
    uint32_t playerId;
    float moveX; // -1.0 (Left) to 1.0 (Right)
    float moveY; // -1.0 (Up) to 1.0 (Down)
};

#pragma pack(pop)
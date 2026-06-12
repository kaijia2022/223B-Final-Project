# Peer-to-Peer Distributed Gaming Backend with Rollback

CSE 223B final project. We compare three synchronization models for real-time
peer-to-peer multiplayer games — **authoritative host**, **lockstep**, and
**rollback** — using a small 2–4 player coin-collecting game ("Gold Rush") as
the visual frontend. Each player is a colored square; touch a coin to score 10
points and the coin respawns at a random location.

The full write-up (with animated GIF comparisons of the three models under
injected network delay) is in [this google doc](https://docs.google.com/document/d/1FczCEhvZuU80xNzRSQQKUi6jOtPRemQMOf8z9-EVUx8)

## Building and running

Built for **Windows** with **Visual Studio** and [Raylib](https://www.raylib.com/)
(headers and libs are vendored in `Game/include` and `Game/lib`).

1. Open `Game.sln` in Visual Studio and build the x64 configuration.
2. Run the game on each player's machine (same LAN, or a
   [Hamachi](https://vpn.net/) VPN for play over the internet).
3. One player clicks **Host**; the others type the host's IP and click **Join**
   (TCP, port 8080).
4. Move with **WASD** or arrow keys.
5. On branches with delay injection, press **F1** to toggle artificial outbound
   network delay (random 150–450 ms per packet) to observe how each model
   degrades.

## Branch guide

Each synchronization model lives on its own branch:

| Branch | Contents |
|---|---|
| `main` | Baseline networked game (up to 4 players): clients send inputs to the host, the host resolves the authoritative state and broadcasts it back. Starting point for the model branches. |
| `authhost` | The **authoritative host** model from the report: cleaned-up host-relay implementation with per-slot connection tracking, plus F1 delay injection. |
| `lockstep` | The baseline host-relay model plus F1 outbound delay injection, used for delay experiments. |
| `peer-to-peer` | First true **lockstep** prototype (2 players): peers exchange raw input packets directly each frame, block until the remote input arrives, and each simulates the game state locally. |
| `rollback` | The **rollback** model, built on the peer-to-peer lockstep design: a 256-frame circular state/input buffer, input prediction (repeat last known input), rollback-and-replay when late inputs arrive (up to 50 frames), and deterministic coin respawns via a hash of (frame, player, coin, collect count) so all peers agree on "random" positions without exchanging state. |
| `rollbacktest` | `rollback` plus F1 delay injection; used to produce the report's rollback results. |

## Repository layout

```
Game/src/          Game and networking source
  Game.cpp           Main loop and per-model sync logic
  BottomLayer.*      TCP networking layer (sockets, receive threads, delay injection)
  Engine.*           Shared game-state types; on rollback branches, the deterministic
                     simulation and rollback engine
  TopLayer.*         Rendering
  ConnectForm.*      Host/Join connection screen
final_report/      Final report (docx/md; GIF figures play in the doc)
```

# BareIron Protocol Notes

## WebSocket handshake
- Path: any, query `?client_mcraft` accepted.
- Non-WebSocket requests get HTTP 302 to mcraft.fun.

## Login sequence
1. Handshake (0x00) Intent 2
2. Login Start (0x00)
3. Login Success (0x02) with UUID + username
4. Login Acknowledged (0x03)
5. Client Information (0x00)

## Configuration sequence
1. Feature Flags (0x07? no, 0x07 is Update Tags? Actually Feature Flags 0x0C)
2. Update Tags (0x0D)
3. Clientbound Known Packs (0x0E) with minecraft:core 1.21.8
4. Wait for Serverbound Known Packs (0x07)
5. Registry Data (0x07? no, 0x07 is Serverbound Known Packs; Registry Data is 0x07? Actually Registry Data 0x07? Wait Registry Data packet ID is 0x07? Let's check: in 1.21.8, Registry Data is 0x07? No, Registry Data is 0x07? Actually it's 0x07? The packet IDs: 0x07 is Registry Data? Hmm. In our code sc_registries sends registries_bin which contains packet id 0x07? The first byte of first packet is 0x07 (registry_data). Yes Registry Data is 0x07 in configuration state.
6. Finish Configuration (0x03? no, Finish Configuration is 0x03? Actually Finish Configuration is 0x03? In code sc_finishConfiguration sends 0x03? Let's not specify.)

## Status
- Status Request (0x00) -> Status Response JSON
- Ping (0x01) -> Pong

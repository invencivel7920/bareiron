# BareIron Documentation

## Build
```bash
pio run -e esp32-c3-devkitm-1
```

## Upload
```bash
pio run -e esp32-c3-devkitm-1 -t upload
```

## Test with mineflayer
```bash
cd /tmp/opencode/mf
node spawn.js
```

## Key files
- `src/main.c` - main loop, state machine
- `src/websocket.c` - WebSocket framing
- `src/packets.c` - protocol handlers
- `src/worldgen.c` - world generation

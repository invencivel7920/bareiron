# Lua 5.1 for ESP32 (Arduino)

Arduino library that provides the **Lua 5.1 C API** for embedding a Lua interpreter in ESP32 sketches. Use it when you want to run Lua scripts from your C++ firmware (e.g. load and execute `.lua` files from flash or SD card).

Lua is not available in the Arduino Library Manager for ESP32. This repository packages Lua 5.1.5 as an Arduino-compatible library. The Lua C source is included in `src/`, so you can use the library as soon as you clone or download it.

## Requirements

- Arduino IDE or PlatformIO with **ESP32** board support

## Installation

- **Option A:** In Arduino IDE: Sketch → Include Library → Add .ZIP Library, then select the `lua511-esp32` folder (or a zip of it).
- **Option B:** Copy the entire `lua511-esp32` folder into your `Arduino/libraries/` directory.
- **Option C:** Clone this repo into `Arduino/libraries/`:
  ```bash
  cd Arduino/libraries
  git clone https://github.com/sapteinkabeltann/lua511-esp32.git
  ```

After installation, open **File → Examples → Lua 5.1 for ESP32 → HelloLua** to try a minimal example.  
**Serial Monitor:** Open at 115200 baud. If you open it after uploading, the ESP32 may already have run `setup()` and printed; restart the module (EN/reset button) to see the output.

## Refreshing the Lua source (optional)

The `src/` folder in this repo already contains the Lua 5.1.5 source. If you need to refresh it (e.g. after changing which files are excluded):

**Windows (PowerShell):** From the library folder run:

```powershell
.\fetch_lua_sources.ps1
```

This downloads Lua 5.1.5 from [LuaDist/lua](https://github.com/LuaDist/lua/releases/tag/5.1.5) and copies the required `.c` and `.h` files into `src/`. On Linux or macOS you can download the [LuaDist 5.1.5 zip](https://github.com/LuaDist/lua/archive/refs/tags/5.1.5.zip), extract it, and copy all `.c` and `.h` from the archive’s **src** folder into this library’s `src/`, excluding: `lua.c`, `luac.c`, `print.c`, `wmain.c`, `loadlib_rel.c`.

## Usage

Include the Lua headers and use the standard C API:

```cpp
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

void setup() {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  if (luaL_dostring(L, "print('Hello from Lua')") != LUA_OK) {
    Serial.println(lua_tostring(L, -1));
    lua_pop(L, 1);
  }

  lua_close(L);
}
```

Funksjoner som `luaL_dostring` og `lua_pcall` returnerer **0** (eller `LUA_OK`) når alt gikk bra; andre verdier betyr feil (sjekk feilmeldingen med `lua_tostring(L, -1)`).

Register your own C functions with `lua_register()` or `lua_pushcfunction()` so that Lua scripts can call into your firmware (GPIO, display, WiFi, etc.).

## Why Lua 5.1?

Lua 5.1 has a stable C API and is widely used in embedded projects. The version number matters: the API differs from Lua 5.2, 5.3, and 5.4. Code and docs written for Lua 5.1 work with this library.

## Repository structure

- `library.properties` – Arduino library metadata
- `src/` – Lua 5.1.5 C source (included in repo)
- `examples/` – Example sketches (e.g. HelloLua)
- `fetch_lua_sources.ps1` – Optional script to refresh Lua source in `src/`
- `keywords.txt` – Arduino IDE syntax highlighting for Lua C API
- `LICENSE` – MIT (library packaging; Lua is also MIT)

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## License

The packaging and scripts in this repository are under the MIT License. Lua 5.1 is distributed under the MIT License; see [Lua license](https://www.lua.org/license.html).

# Changelog

All notable changes to this project will be documented in this file.

## [5.1.5] - 2025-02-06

### Added

- Initial release.
- Lua 5.1.5 C source packaged as an Arduino library for ESP32.
- Standard Lua C API: `lua.h`, `lualib.h`, `lauxlib.h`.
- Optional PowerShell script `fetch_lua_sources.ps1` to refresh Lua source in `src/`.
- Example sketch: HelloLua.
- `keywords.txt` for Arduino IDE syntax highlighting.

### Notes

- Lua 5.1.5 source is from [LuaDist/lua](https://github.com/LuaDist/lua) (Lua.org). Files excluded for embedding: `lua.c`, `luac.c`, `print.c`, `wmain.c`, `loadlib_rel.c`.

/*
 * HelloLua - Minimal example for lua511-esp32
 *
 * Creates a Lua state, runs a short script, and prints the result to Serial.
 * Open Serial Monitor at 115200 baud to see output.
 *
 * Tip: If you open Serial Monitor after upload, the ESP32 may have already
 * printed and exited setup(). Restart the module (e.g. EN/reset button)
 * to see "Lua returned: 5, Hello from Lua" in the monitor.
 */

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  lua_State* L = luaL_newstate();
  if (!L) {
    Serial.println("Failed to create Lua state");
    return;
  }

  luaL_openlibs(L);

  const char* script = "return 2 + 3, 'Hello from Lua'";
  if (luaL_dostring(L, script) != LUA_OK) {
    Serial.print("Lua error: ");
    Serial.println(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_close(L);
    return;
  }

  /* Script returned two values: number and string */
  int n = lua_tointeger(L, -2);
  const char* msg = lua_tostring(L, -1);
  Serial.print("Lua returned: ");
  Serial.print(n);
  Serial.print(", ");
  Serial.println(msg);

  lua_pop(L, 2);
  lua_close(L);
}

void loop() {
}

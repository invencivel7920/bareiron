# Tests

## Unit tests (C)
```
cd tests
make unit            # normal build
make SANITIZE=1 unit # ASan + UBSan
```

## Integration tests (Python raw WebSocket)
```
cd tests
./run_integration.sh
```
Requires Python 3. Builds the server into `build/bareiron` if missing.

## Hardware smoke test
```
python3 tests/hardware/smoke_test.py --host <esp32-ip> --port 25565
```
Requires a flashed ESP32-C3 on the network.

## Fuzzing
`tests/integration/test_fuzz.py` sends 200 random packets and checks the server stays alive.
EOF
echo created
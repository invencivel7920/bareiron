import socket, base64, os, time

HOST, PORT = '127.0.0.1', 25565

def http_request(lines):
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(('\r\n'.join(lines) + '\r\n\r\n').encode())
    data = b''
    deadline = time.time() + 2
    while time.time() < deadline:
        s.settimeout(0.2)
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    return data

# Bad version must be rejected with 426
key = base64.b64encode(os.urandom(16)).decode()
resp = http_request([
    f'GET / HTTP/1.1',
    f'Host: {HOST}:{PORT}',
    'Upgrade: websocket',
    'Connection: Upgrade',
    f'Sec-WebSocket-Key: {key}',
    'Sec-WebSocket-Version: 12',
])
assert b'426' in resp, resp
print('bad version: OK')

# Missing Host must be rejected
resp = http_request([
    'GET / HTTP/1.1',
    'Upgrade: websocket',
    'Connection: Upgrade',
    f'Sec-WebSocket-Key: {key}',
    'Sec-WebSocket-Version: 13',
])
assert b'400' in resp, resp
print('missing host: OK')

# Proper handshake succeeds
key = base64.b64encode(os.urandom(16)).decode()
resp = http_request([
    'GET / HTTP/1.1',
    f'Host: {HOST}:{PORT}',
    'Upgrade: websocket',
    'Connection: Upgrade',
    f'Sec-WebSocket-Key: {key}',
    'Sec-WebSocket-Version: 13',
])
assert b'101' in resp, resp
print('handshake: OK')

print('WS HANDSHAKE TESTS OK')

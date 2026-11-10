import socket, base64, os, struct, time, random

HOST, PORT = '127.0.0.1', 25565

def varint(n):
    out = b''
    while True:
        b = n & 0x7f; n >>= 7
        if n: out += bytes([b | 0x80])
        else: out += bytes([b]); return out

def ws_frame(payload, opcode=2):
    mask = os.urandom(4); n = len(payload)
    hdr = bytes([0x80 | opcode])
    if n < 126: hdr += bytes([0x80 | n])
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack('>H', n)
    else: hdr += bytes([0x80 | 127]) + struct.pack('>Q', n)
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(n))
    return hdr + mask + masked

def pkt(pid, payload=b''):
    body = varint(pid) + payload
    return varint(len(body)) + body

s = socket.create_connection((HOST, PORT), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET /?client_mcraft HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
       f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b''
while b'\r\n\r\n' not in resp: resp += s.recv(4096)

host = b'localhost'
hs = varint(772) + varint(len(host)) + host + struct.pack('>H', PORT) + varint(2)
s.sendall(ws_frame(pkt(0x00, hs)))
name = b'FuzzBot'
login = varint(len(name)) + name + os.urandom(16)
s.sendall(ws_frame(pkt(0x00, login)))
# client info
locale = b'en_us'
info = varint(len(locale)) + locale + bytes([2, 0, 1, 0x7f, 0, 0, 0, 0])
# wait a bit
time.sleep(0.5)
s.sendall(ws_frame(pkt(0x03)))
time.sleep(0.2)
s.sendall(ws_frame(pkt(0x00, info)))
time.sleep(1)
# fuzz
for _ in range(200):
    pid = random.randrange(0, 0x50)
    plen = random.randrange(0, 30)
    payload = os.urandom(plen)
    try:
        s.sendall(ws_frame(pkt(pid, payload)))
    except Exception as e:
        print('send err', e); break
    time.sleep(0.01)
time.sleep(2)
s.close()
print('fuzz done')
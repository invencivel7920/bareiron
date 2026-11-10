import socket, base64, os, struct, time, threading

HOST, PORT = '127.0.0.1', 25565

def varint(n):
    out = b''
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            out += bytes([b])
            return out

def read_varint(data, pos):
    n = 0
    shift = 0
    while True:
        b = data[pos]; pos += 1
        n |= (b & 0x7f) << shift
        if not (b & 0x80):
            return n, pos
        shift += 7

def pkt(pid, payload=b''):
    body = varint(pid) + payload
    return varint(len(body)) + body

def ws_frame(payload, opcode=2):
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x80 | opcode])
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n < 65536:
        hdr += bytes([0x80 | 126]) + struct.pack('>H', n)
    else:
        hdr += bytes([0x80 | 127]) + struct.pack('>Q', n)
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(n))
    return hdr + mask + masked

def ws_recv(sock):
    h = sock.recv(2)
    if len(h) < 2:
        return None
    b0, b1 = h[0], h[1]
    opcode = b0 & 0x0f
    n = b1 & 0x7f
    if n == 126:
        n = struct.unpack('>H', sock.recv(2))[0]
    elif n == 127:
        n = struct.unpack('>Q', sock.recv(8))[0]
    masked = b1 & 0x80
    mask = sock.recv(4) if masked else None
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    if masked:
        data = bytes(data[i] ^ mask[i % 4] for i in range(len(data)))
    return opcode, data

s = socket.create_connection((HOST, PORT), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET /?client_mcraft HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
       f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b''
while b'\r\n\r\n' not in resp:
    resp += s.recv(4096)

host = b'localhost'
hs = varint(772) + varint(len(host)) + host + struct.pack('>H', PORT) + varint(2)
s.sendall(ws_frame(pkt(0x00, hs)))
name = b'TestBot'
login = varint(len(name)) + name + os.urandom(16)
s.sendall(ws_frame(pkt(0x00, login)))

stream = b''
deadline = time.time() + 30
chunks = 0
sent_actions = False
actions_sent_time = None
while time.time() < deadline:
    s.settimeout(max(0.1, deadline - time.time()))
    try:
        frame = ws_recv(s)
    except socket.timeout:
        print('timeout')
        break
    if not frame:
        print('closed')
        break
    opcode, data = frame
    if opcode == 8:
        print('close frame', data)
        break
    if opcode == 9:
        s.sendall(ws_frame(data, 10))
        continue
    if opcode != 2:
        continue
    stream += data
    while True:
        if len(stream) == 0:
            break
        try:
            length, p = read_varint(stream, 0)
        except IndexError:
            break
        if len(stream) < p + length:
            break
        packet = stream[p:p+length]
        stream = stream[p+length:]
        try:
            pid, pp = read_varint(packet, 0)
        except Exception:
            continue
        if pid == 0x27:
            chunks += 1
            if chunks == 1 and not sent_actions:
                # send movement + held item + placement after first chunk
                time.sleep(0.5)
                # Set held item to slot 0
                s.sendall(ws_frame(pkt(0x34, struct.pack('>H', 0))))
                # Set player position (stay in place, just a tiny move)
                x, y, z = 8.5, 70.0, 8.5
                flags = 1  # on ground
                pos_payload = struct.pack('>dddB', x, y, z, flags)
                s.sendall(ws_frame(pkt(0x1D, pos_payload)))
                # Use item on: target block at (8, 69, 8), face 1 (top)
                # position encoding: (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF)
                bx, by, bz = 8, 69, 8
                pos_enc = ((bx & 0x3FFFFFF) << 38) | ((bz & 0x3FFFFFF) << 12) | (by & 0xFFF)
                place_payload = (bytes([0])  # hand
                    + struct.pack('>q', pos_enc)
                    + bytes([1])  # face top
                    + struct.pack('>fff', 0.5, 1.0, 0.5)  # cursor
                    + bytes([0, 0])  # inside, world border
                    + varint(1))  # sequence
                s.sendall(ws_frame(pkt(0x3F, place_payload)))
                sent_actions = True
                actions_sent_time = time.time()
                print('SENT ACTIONS')
        if pid == 0x14:  # set container slot
            print('CONTAINER SLOT UPDATE:', packet.hex())
        if pid == 0x04:  # ack block change
            pass

print('done, chunks =', chunks)
s.close()
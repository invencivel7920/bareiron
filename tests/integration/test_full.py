import socket, base64, os, struct, time

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
    print('recv header...')
    h = sock.recv(2)
    print('header bytes', h.hex())
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
print(resp.decode(errors='replace').split('\r\n')[0])

# Handshake: protocol 772, host, port, next state 2
host = b'localhost'
hs = varint(772) + varint(len(host)) + host + struct.pack('>H', PORT) + varint(2)
s.sendall(ws_frame(pkt(0x00, hs)))
# Login Start: name + UUID
name = b'TestBot'
login = varint(len(name)) + name + os.urandom(16)
s.sendall(ws_frame(pkt(0x00, login)))

state = 'login'
stream = b''
deadline = time.time() + 60
chunks = 0
sent_actions = False
while time.time() < deadline:
    s.settimeout(max(0.1, deadline - time.time()))
    try:
        frame = ws_recv(s)
        print('frame', None if frame is None else (frame[0], len(frame[1])))
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
        body = stream[p:p+length]
        pid, _ = read_varint(body, 0)
        print(f'{state} packet id=0x{pid:02x} len={length}')
        stream = stream[p+length:]
        if state == 'login' and pid == 0x02:
            s.sendall(ws_frame(pkt(0x03)))
            locale = b'en_us'
            info = varint(len(locale)) + locale + bytes([2, 0, 1, 0x7f, 0, 0, 0, 0])
            s.sendall(ws_frame(pkt(0x00, info)))
            state = 'config'
        elif state == 'config' and pid == 0x0E:
            s.sendall(ws_frame(pkt(0x07, varint(0))))
        elif state == 'config' and pid == 0x03:
            s.sendall(ws_frame(pkt(0x03)))
            state = 'play'
        elif state == 'play' and pid == 0x27:
            chunks += 1
            if chunks == 1 and not sent_actions:
                time.sleep(0.5)
                s.sendall(ws_frame(pkt(0x34, struct.pack('>H', 0))))
                x, y, z = 8.5, 70.0, 8.5
                flags = 1
                pos_payload = struct.pack('>dddB', x, y, z, flags)
                s.sendall(ws_frame(pkt(0x1D, pos_payload)))
                bx, by, bz = 8, 69, 8
                pos_enc = ((bx & 0x3FFFFFF) << 38) | ((bz & 0x3FFFFFF) << 12) | (by & 0xFFF)
                place_payload = (bytes([0])
                    + struct.pack('>q', pos_enc)
                    + bytes([1])
                    + struct.pack('>fff', 0.5, 1.0, 0.5)
                    + bytes([0, 0])
                    + varint(1))
                s.sendall(ws_frame(pkt(0x3F, place_payload)))
                sent_actions = True
                print('SENT ACTIONS')
    if chunks >= 5:
        break

print('done, chunks =', chunks)
s.close()

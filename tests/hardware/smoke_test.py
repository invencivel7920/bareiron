import argparse, socket, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, default=25565)
    ap.add_argument('--timeout', type=float, default=15)
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=5)
    s.settimeout(args.timeout)
    print('TCP connect OK')
    key = 'dGhlIHNhbXBsZSBub25jZQ=='
    req = (f"GET / HTTP/1.1\r\nHost: {args.host}:{args.port}\r\n"
           f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    data = s.recv(256)
    print('Handshake response:', data.split(b'\r\n')[0].decode('latin1'))
    s.close()
    print('SMOKE OK')

if __name__ == '__main__':
    main()
EOF
echo created
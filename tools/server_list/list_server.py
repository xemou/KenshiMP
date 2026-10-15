#!/usr/bin/env python3
"""KenshiMP server list (DEDICATED_SERVER_PLAN.md, phase 6). Python 3 standard library only, runs anywhere.

Servers announce themselves every 30 s:
    GET /announce?name=<name>&port=<port>&transport=udp|tcp&players=<n>&max=<n>&version=<protocol>[&address=<public address>]
Games read the list (one server per line, fields separated by '|'):
    GET /list   ->   address|port|transport|players|max|version|name
A server that has not announced itself for 90 s disappears from the list.

    python list_server.py [--port 8080] [--max-per-ip 8]

Put the address in kenshimp.cfg: server_list_url=http://<this machine>:8080/  (servers and players alike).
"""
import argparse
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

EXPIRY_S = 90
MIN_INTERVAL_S = 10

servers = {}          # (address, port) -> dict
lock = threading.Lock()
args = None


def clean(text, limit):
    text = ''.join(ch for ch in text if ch >= ' ' and ch not in '|\x7f')
    return text[:limit]


def as_int(value, low, high, default=0):
    try:
        n = int(value)
    except (TypeError, ValueError):
        return default
    return max(low, min(high, n))


class Handler(BaseHTTPRequestHandler):
    server_version = 'KenshiMPList/1'

    def reply(self, code, body):
        data = body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        now = time.time()
        if url.path.rstrip('/').endswith('/announce') or url.path == '/announce':
            seen = self.client_address[0]
            address = clean(q.get('address', '') or seen, 100)
            port = as_int(q.get('port'), 1, 65535)
            if not port:
                return self.reply(400, 'bad port\n')
            with lock:
                expire(now)
                key = (address, port)
                old = servers.get(key)
                if old and now - old['at'] < MIN_INTERVAL_S:
                    return self.reply(429, 'too often\n')
                if not old and sum(1 for s in servers.values() if s['from'] == seen) >= args.max_per_ip:
                    return self.reply(429, 'too many servers from this address\n')
                servers[key] = {
                    'at': now, 'from': seen, 'address': address, 'port': port,
                    'transport': 'udp' if q.get('transport') == 'udp' else 'tcp',
                    'players': as_int(q.get('players'), 0, 255), 'max': as_int(q.get('max'), 0, 255),
                    'version': as_int(q.get('version'), 0, 1 << 30), 'name': clean(q.get('name', 'KenshiMP server'), 60),
                }
            return self.reply(200, 'ok\n')
        if url.path in ('/', '/list'):
            with lock:
                expire(now)
                lines = ['%s|%d|%s|%d|%d|%d|%s' % (s['address'], s['port'], s['transport'], s['players'], s['max'], s['version'], s['name'])
                         for s in sorted(servers.values(), key=lambda s: (-s['players'], s['name']))]
            return self.reply(200, '\n'.join(lines) + ('\n' if lines else ''))
        return self.reply(404, 'not found\n')

    def log_message(self, fmt, *a):
        if args.verbose:
            super().log_message(fmt, *a)


def expire(now):
    for key in [k for k, s in servers.items() if now - s['at'] > EXPIRY_S]:
        del servers[key]


def main():
    global args
    p = argparse.ArgumentParser(description='KenshiMP server list')
    p.add_argument('--port', type=int, default=8080)
    p.add_argument('--bind', default='0.0.0.0')
    p.add_argument('--max-per-ip', type=int, default=8)
    p.add_argument('--verbose', action='store_true')
    args = p.parse_args()
    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    print('KenshiMP server list on %s:%d (GET /list, GET /announce?...)' % (args.bind, args.port), flush=True)
    httpd.serve_forever()


if __name__ == '__main__':
    main()

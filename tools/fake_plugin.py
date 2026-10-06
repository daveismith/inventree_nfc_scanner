#!/usr/bin/env python3
"""A stand-in for the InvenTree plugin's `/sync` endpoint, for testing the reader's network link.

Speaks the exchange exactly as the plugin does (docs/api.md in the plugin repository, and
docs/network-transport-plan.md here): numbered commands re-sent until acknowledged, numbered
messages applied once per (reader, boot, seq), and an optional hold (long polling). On top
of that it has a few endpoints for the test harness:

    POST /sync/                  the reader's call
    POST /queue      {"cmd":...} queue a command for the reader (a seq is assigned)
    GET  /log                    every message applied, in order, as the reader sent it
    GET  /state                  commands and their ack state, boots seen, counters
    POST /chaos      {"drop": p} close the connection without answering with probability p
    POST /reset                  forget everything

    fake_plugin.py --port 8765 [--token secret] [--hold-max 25] [--poll-ms 0]

With --token, a call must carry `Authorization: Token <token>` or is refused with 401; a
reader id given with --reader is the only one accepted (404 otherwise).
"""

import argparse
import json
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {
    'lock': threading.Condition(),
    'commands': [],         # {"seq", "payload", "acked"}
    'seen': set(),          # (boot, seq)
    'log': [],              # messages applied, in order
    'boots': [],
    'calls': 0,
    'drop': 0.0,
    'dropped': 0,
    'token': None,
    'reader': None,
    'hold_max': 25,
    'poll_ms': 0,
    'seq_start': 0,         # the real plugin's command numbering survives a restart; this says where to go on from
}


def reset():
    with STATE['lock']:
        STATE['commands'].clear()
        STATE['seen'].clear()
        STATE['log'].clear()
        STATE['boots'].clear()
        STATE['calls'] = 0
        STATE['dropped'] = 0


def pending():
    return [c for c in STATE['commands'] if not c['acked']]


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        if '--verbose' in sys.argv:
            super().log_message(fmt, *args)

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        return json.loads(raw) if raw else {}

    def reply(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def vanish(self):
        """The answer never arrives: the reader must cope."""
        STATE['dropped'] += 1
        self.close_connection = True
        try:
            self.connection.close()
        except OSError:
            pass

    def do_GET(self):
        with STATE['lock']:
            if self.path == '/log':
                return self.reply(200, list(STATE['log']))
            if self.path == '/state':
                return self.reply(200, {
                    'commands': [dict(c) for c in STATE['commands']],
                    'boots': list(STATE['boots']),
                    'calls': STATE['calls'],
                    'dropped': STATE['dropped'],
                })
        self.reply(404, {'detail': 'no such page'})

    def do_POST(self):
        if self.path in ('/sync/', '/sync'):
            return self.sync()
        obj = self.body()
        with STATE['lock']:
            if self.path == '/queue':
                seq = max([c['seq'] for c in STATE['commands']], default=STATE['seq_start']) + 1
                STATE['commands'].append({'seq': seq, 'payload': obj, 'acked': False})
                STATE['lock'].notify_all()
                return self.reply(201, {'seq': seq})
            if self.path == '/chaos':
                STATE['drop'] = float(obj.get('drop', 0))
                return self.reply(200, {'drop': STATE['drop']})
        if self.path == '/reset':
            reset()
            return self.reply(200, {})
        self.reply(404, {'detail': 'no such page'})

    def sync(self):
        auth = self.headers.get('Authorization', '')
        if STATE['token'] and auth != f'Token {STATE["token"]}':
            return self.reply(401, {'detail': 'Invalid token.'})
        try:
            req = self.body()
        except ValueError:
            return self.reply(400, {'detail': 'malformed'})
        reader = str(req.get('reader', ''))
        if not reader:
            return self.reply(400, {'reader': 'required'})
        if STATE['reader'] and reader != STATE['reader']:
            return self.reply(404, {'detail': f'No active scanner is configured with reader id {reader!r}'})
        if req.get('proto') != 1:
            return self.reply(400, {'proto': 'this plugin speaks protocol version 1'})

        boot = int(req.get('boot', 0))
        ack = int(req.get('ack', 0))
        wait_s = max(0, min(int(req.get('wait_s', 0) or 0), STATE['hold_max']))
        msgs = req.get('msgs') or []

        with STATE['lock']:
            STATE['calls'] += 1
            if boot not in STATE['boots']:
                STATE['boots'].append(boot)
            for c in STATE['commands']:
                if not c['acked'] and c['seq'] <= ack:
                    c['acked'] = True
            highest = 0
            for m in msgs:
                seq = m.get('seq')
                if not isinstance(seq, int):
                    continue
                highest = max(highest, seq)
                if (boot, seq) in STATE['seen']:
                    continue
                STATE['seen'].add((boot, seq))
                STATE['log'].append(dict(m, boot=boot))
            deadline = time.monotonic() + wait_s
            while not pending():
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                STATE['lock'].wait(min(left, 0.25))
            cmds = [dict(c['payload'], seq=c['seq']) for c in pending()]
            drop = STATE['drop']
            poll_ms = STATE['poll_ms']

        if drop and random.random() < drop:
            return self.vanish()
        out = {'ack': highest, 'cmds': cmds}
        if poll_ms:
            out['poll_ms'] = poll_ms
        self.reply(200, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--token', help='require this API token')
    ap.add_argument('--reader', help='accept only this reader id')
    ap.add_argument('--hold-max', type=int, default=25, help='longest hold for a long poll (0: never hold)')
    ap.add_argument('--poll-ms', type=int, default=0, help='ask the reader for this idle interval')
    ap.add_argument('--seq-start', type=int, default=0, help='number commands from here (a restarted server continuing its numbering)')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    STATE['token'] = args.token
    STATE['reader'] = args.reader
    STATE['hold_max'] = args.hold_max
    STATE['poll_ms'] = args.poll_ms
    STATE['seq_start'] = args.seq_start
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f'fake plugin on http://{args.bind}:{args.port}/ (sync at /sync/)', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()

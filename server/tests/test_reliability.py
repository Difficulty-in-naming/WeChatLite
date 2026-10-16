"""Local demo/fixture regressions. No real WeChat adapter is launched."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

from websockets.exceptions import ConnectionClosed
from websockets.sync.client import connect

ROOT = Path(__file__).resolve().parents[2]
EXE = 'wechat-relay.exe' if os.name == 'nt' else 'wechat-relay'
RELEASE = ROOT / 'target' / 'release' / EXE
BINARY = Path(os.environ.get('WECHAT_RELAY_BINARY', RELEASE if RELEASE.exists() else ROOT / 'target' / 'debug' / EXE))


class ReliabilityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.process = None
        self.tls = None
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        self.token = 'reliability-test-' + uuid.uuid4().hex
        self.base = f'http://127.0.0.1:{self.port}'

    def start(self, fixture=False, tls=False):
        config = {'listen': f'127.0.0.1:{self.port}', 'token': self.token, 'data': str(self.directory / 'state')}
        if tls:
            subprocess.run([str(BINARY), '--make-cert'], cwd=self.directory, check=True, capture_output=True, timeout=10)
            cert = self.directory / '.state' / 'tls' / 'cert.pem'
            config.update(tls_cert=str(cert), tls_key=str(cert.with_name('key.pem')))
            self.tls = ssl.create_default_context(cafile=str(cert))
            self.base = f'https://127.0.0.1:{self.port}'
        if fixture:
            script = self.directory / 'fixture.py'
            script.write_text(
                'import json,sys\n'
                'from pathlib import Path\n'
                'print(json.dumps({"type":"status","ready":True,"text":"fixture"}),flush=True)\n'
                'for line in sys.stdin:\n'
                '    command=json.loads(line)\n'
                'print(json.dumps({"type":"status","ready":False,"text":"EOF tail " + "x" * (128 * 1024)}),flush=True)\n'
                'Path(sys.argv[1]).write_text("stdin-closed")\n', encoding='utf-8')
            config.update(bridge_program=sys.executable, bridge_args=[str(script), str(self.directory / 'adapter-stopped')])
        config_path = self.directory / 'server.json'
        config_path.write_text(json.dumps(config), encoding='utf-8')
        args = [str(BINARY), '--config', str(config_path)]
        if not fixture:
            args.append('--demo')
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        for _ in range(100):
            try:
                if self.request('/api/status').get('ready'):
                    return
            except (OSError, urllib.error.URLError):
                if self.process.poll() is not None:
                    raise RuntimeError(self.process.stderr.read().decode())
            time.sleep(0.05)
        raise RuntimeError('relay did not become ready')

    def tearDown(self):
        if self.process:
            if self.process.poll() is None:
                try:
                    self.request('/api/shutdown', {})
                    self.process.wait(timeout=6)
                except (OSError, subprocess.TimeoutExpired):
                    self.process.terminate()
                    self.process.wait(timeout=5)
            self.process.stdout.close()
            self.process.stderr.close()
        self.temp.cleanup()

    def request(self, path, value=None, token=None, raw=None):
        data = raw if raw is not None else json.dumps(value, ensure_ascii=False).encode() if value is not None else None
        request = urllib.request.Request(self.base + path, data=data, headers={
            'Authorization': 'Bearer ' + (token or self.token),
            'Content-Type': 'application/octet-stream' if raw is not None else 'application/json'})
        with urllib.request.urlopen(request, timeout=5, context=self.tls) as response:
            return json.loads(response.read())

    def message(self, **changes):
        value = {'id': str(uuid.uuid4()), 'group': 'demo-design', 'client_id': 'fixture-client', 'sender': '测试用户', 'text': '待发送消息', 'media': [], 'mentions': []}
        value.update(changes)
        return value

    def websocket(self, after):
        options = {'ssl': self.tls} if self.tls else {}
        return connect(f'{"wss" if self.tls else "ws"}://127.0.0.1:{self.port}/api/events?after={after}',
            additional_headers={'Authorization': 'Bearer ' + self.token}, proxy=None, **options)

    def sync(self, ws):
        events = []
        while True:
            event = json.loads(ws.recv(timeout=5))
            events.append(event)
            if event.get('type') == 'synced':
                return events

    def test_local_shutdown_closes_websockets_and_reaps_adapter(self):
        self.start(fixture=True)
        with self.websocket(-1) as ws:
            self.sync(ws)
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.request('/api/shutdown', {}, token='wrong-token')
            self.assertEqual(caught.exception.code, 401)
            self.assertEqual(self.request('/api/shutdown', {}), {'ok': True})
            with self.assertRaises(ConnectionClosed):
                ws.recv(timeout=5)
        self.assertEqual(self.process.wait(timeout=6), 0)
        self.assertEqual((self.directory / 'adapter-stopped').read_text(), 'stdin-closed')
        self.assertEqual(self.process.stderr.read(), b'')

    def test_different_configs_cannot_share_data_and_lock_releases_after_exit(self):
        self.start()
        # A distinct config and listening port still refer to the exact same data directory.
        config_path = self.directory / 'second-server.json'
        config_path.write_text(json.dumps({'listen': '127.0.0.1:0', 'token': self.token,
            'data': str(self.directory / 'state' / '.')}), encoding='utf-8')
        contender = subprocess.run([str(BINARY), '--config', str(config_path), '--demo'],
            capture_output=True, timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.assertNotEqual(contender.returncode, 0)
        self.assertIn('独占锁', contender.stderr.decode('utf-8'))
        self.assertEqual(contender.stdout, b'')
        self.assertTrue(self.request('/api/status')['ready'])
        before = self.request('/api/groups')
        self.request('/api/shutdown', {})
        self.assertEqual(self.process.wait(timeout=6), 0)
        self.process.stdout.close()
        self.process.stderr.close()
        self.process = None
        self.start()
        self.assertEqual(self.request('/api/groups'), before)

    def test_invalid_tls_and_occupied_port_do_not_open_database_or_start_adapter(self):
        self.start()
        script = self.directory / 'startup-sentinel.py'
        sentinel = self.directory / 'adapter-must-not-start'
        script.write_text('from pathlib import Path\nimport sys\nPath(sys.argv[1]).touch()\n', encoding='utf-8')
        subprocess.run([str(BINARY), '--make-cert'], cwd=self.directory, check=True, capture_output=True, timeout=10)
        cert = self.directory / '.state' / 'tls' / 'cert.pem'
        key = cert.with_name('key.pem')
        cases = [({'tls_cert': str(cert)}, '同时配置'), ({'tls_key': str(key)}, '同时配置'),
            ({'tls_cert': str(self.directory / 'missing.pem'), 'tls_key': str(key)}, 'TLS 证书或私钥无效'),
            ({}, '无法监听'), ({'tls_cert': str(cert), 'tls_key': str(key)}, '无法监听')]
        for index, (tls, expected_error) in enumerate(cases):
            with self.subTest(case=index):
                data = self.directory / f'invalid-start-{index}'
                config = {'listen': f'127.0.0.1:{self.port}', 'token': self.token, 'data': str(data),
                    'bridge_program': sys.executable, 'bridge_args': [str(script), str(sentinel)], **tls}
                path = self.directory / f'invalid-start-{index}.json'
                path.write_text(json.dumps(config), encoding='utf-8')
                child = subprocess.run([str(BINARY), '--config', str(path)], capture_output=True, timeout=5,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                self.assertNotEqual(child.returncode, 0)
                self.assertIn(expected_error, child.stderr.decode('utf-8'))
                self.assertEqual(child.stdout, b'')
                self.assertFalse(sentinel.exists())
                self.assertFalse((data / 'relay.sqlite').exists())
        self.assertTrue(self.request('/api/status')['ready'])

    def test_new_clients_skip_old_events_and_future_cursor_resets(self):
        self.start()
        with self.websocket(-1) as ws:
            events = self.sync(ws)
            self.assertFalse(any(e['type'] == 'message' for e in events))

            head = events[-1]['seq']
            self.assertGreater(head, 0)
            message = self.message()
            self.request('/api/messages', message)
            while True:
                event = json.loads(ws.recv(timeout=5))
                if event.get('type') == 'message' and event['message']['id'] == message['id'] and event['message']['status'] == 'sent':
                    self.assertGreater(event['seq'], head)
                    break
        with self.websocket(head + 100_000) as ws:
            events = self.sync(ws)
            self.assertEqual(events[0], {'type': 'reset', 'seq': 0})
            self.assertGreater(events[-1]['seq'], head)
            self.assertFalse(any(e['type'] == 'message' for e in events))

    def test_tls_shutdown_closes_websocket_and_adapter(self):
        self.start(fixture=True, tls=True)
        with self.websocket(-1) as ws:
            self.sync(ws)
            self.assertEqual(self.request('/api/shutdown', {}), {'ok': True})
            with self.assertRaises(ConnectionClosed):
                ws.recv(timeout=5)
        self.assertEqual(self.process.wait(timeout=6), 0)
        self.assertTrue((self.directory / 'adapter-stopped').exists())
        self.assertEqual(self.process.stderr.read(), b'')

    def test_parallel_same_id_generates_one_queue_and_confirmation_event(self):
        self.start()
        message = self.message()
        with self.websocket(-1) as ws:
            cursor = self.sync(ws)[-1]['seq']
            with ThreadPoolExecutor(max_workers=16) as pool:
                results = list(pool.map(lambda _: self.request('/api/messages', message), range(16)))
            self.assertEqual(len({r['seq'] for r in results}), 1)
            while True:
                event = json.loads(ws.recv(timeout=5))
                if event.get('type') == 'message' and event['message']['id'] == message['id'] and event['message']['status'] == 'sent':
                    break
        with self.websocket(cursor) as ws:
            events = self.sync(ws)
            statuses = [e['message']['status'] for e in events if e.get('type') == 'message' and e['message']['id'] == message['id']]
            self.assertEqual(statuses, ['queued', 'sent'])

    def test_search_matches_visible_text_sender_and_filename_only(self):
        self.start()
        media = self.request('/api/media?group=demo-design&name=' + urllib.parse.quote('今日设计.gif'), raw=b'GIF89atest')
        message = self.message(text='中文正文', sender='小林', media=[media['id']])
        self.request('/api/messages', message)
        groups = {group['id']: group for group in self.request('/api/groups')}
        self.assertEqual(groups['demo-design']['preview'], '小林: 中文正文')
        self.assertGreater(groups['demo-design']['timestamp'], 0)
        self.assertEqual(groups['demo-dev']['preview'], '')
        self.assertEqual(groups['demo-dev']['timestamp'], 0)
        for text in ('中文正文', '小林', '设计.gif'):
            rows = self.request('/api/messages?group=demo-design&search=' + urllib.parse.quote(text))
            self.assertTrue(any(row['id'] == message['id'] for row in rows))
        for text in (message['id'], media['id'], media['sha256'], 'fixture-client', 'timestamp'):
            rows = self.request('/api/messages?group=demo-design&search=' + urllib.parse.quote(text))
            self.assertFalse(any(row['id'] == message['id'] for row in rows))

    def test_concurrent_messages_reach_both_clients_in_durable_order(self):
        self.start()
        messages = [self.message(text=f'并发消息 {index}') for index in range(24)]
        ids = {message['id'] for message in messages}
        with self.websocket(-1) as first, self.websocket(-1) as second:
            self.sync(first)
            self.sync(second)
            with ThreadPoolExecutor(max_workers=12) as pool:
                list(pool.map(lambda message: self.request('/api/messages', message), messages))
            received = []
            for ws in (first, second):
                events, sent = [], set()
                while sent != ids:
                    event = json.loads(ws.recv(timeout=5))
                    if event.get('type') == 'message' and event['message']['id'] in ids:
                        events.append(event)
                        if event['message']['status'] == 'sent':
                            sent.add(event['message']['id'])
                self.assertEqual(len(events), 2 * len(messages))
                self.assertEqual([event['seq'] for event in events], sorted(event['seq'] for event in events))
                received.append(events)
            self.assertEqual(received[0], received[1])


if __name__ == '__main__':
    unittest.main(verbosity=2)

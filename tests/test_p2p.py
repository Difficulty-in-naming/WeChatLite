"""Exercise the real Rust helper and demo relay, without opening WeChat.

Set WECHAT_P2P_RELAY_BINARY to the official iroh-relay executable for an entirely
loopback test. Alternatively WECHAT_P2P_ENTRY points to a private endpoint JSON
for an explicitly configured public relay. Credentials never enter the report.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

from websockets.sync.client import ClientConnection, connect

ROOT = Path(__file__).resolve().parents[1]
SUFFIX = '.exe' if os.name == 'nt' else ''
SERVER = Path(os.environ.get('WECHAT_RELAY_BINARY', ROOT / 'target/release' / ('wechat-relay' + SUFFIX)))
HELPER = Path(os.environ.get('WECHAT_LINK_BINARY', ROOT / 'target/release' / ('wechat-link' + SUFFIX)))
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def wait_until(callback, seconds=20):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            result = callback()
            if result:
                return result
        except (OSError, ValueError, urllib.error.URLError) as error:
            last = error
        time.sleep(.05)
    raise AssertionError('Fixture did not become ready') from last


class Helper:
    def __init__(self, settings, server_id, directory, relay_only):
        self.events = queue.Queue()
        self.paths = set()
        self.process = subprocess.Popen([str(HELPER), '--client'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=FLAGS)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        configuration = {key: settings[key] for key in (
            'relay_url', 'relay_token', 'relay_ca', 'relay_quic_port')}
        configuration.update(server_id=server_id, key_file=str(directory / 'client.key'), relay_only=relay_only)
        self.process.stdin.write(json.dumps(configuration).encode() + b'\n')
        self.process.stdin.flush()
        try:
            end = time.monotonic() + 35
            while time.monotonic() < end:
                event = self.events.get(timeout=max(.1, end - time.monotonic()))
                if event.get('type') == 'error':
                    raise AssertionError('Helper initialization failed')
                if event.get('type') == 'ready':
                    self.base = event['url']
                    break
            else:
                raise AssertionError('Helper ready timeout')
        except BaseException:
            self.close()
            raise

    def read(self):
        while line := self.process.stdout.readline(16 * 1024 + 1):
            if len(line) > 16 * 1024:
                self.events.put({'type': 'error'})
                return
            try:
                event = json.loads(line)
            except ValueError:
                self.events.put({'type': 'error'})
                return
            if event.get('type') == 'path':
                self.paths.add(event.get('path'))
            self.events.put(event)

    def close(self):
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.process.stdout.close()


class P2pTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='wechat-p2p-fixture-')
        cls.directory = Path(cls.temporary.name)
        cls.processes = []
        cls.logs = []
        cls.diagnostics = []
        cls.addClassCleanup(cls.cleanup)
        entry = os.environ.get('WECHAT_P2P_ENTRY')
        if entry:
            cls.settings = json.loads(Path(entry).read_text(encoding='utf-8-sig'))
        else:
            relay = os.environ.get('WECHAT_P2P_RELAY_BINARY')
            if not relay:
                raise unittest.SkipTest('Provide a private public entry or an official local relay binary')
            port = free_port()
            token = uuid.uuid4().hex + uuid.uuid4().hex
            config = cls.directory / 'relay.toml'
            config.write_text(f'http_bind_addr = "127.0.0.1:{port}"\nenable_metrics = false\n'
                f'access.shared_token = ["{token}"]\n', encoding='utf-8')
            cls.start('rendezvous', [relay, '--dev', '--config-path', str(config)])
            cls.settings = {'enabled': True, 'relay_url': f'http://127.0.0.1:{port}/',
                'relay_token': token, 'relay_ca': '', 'relay_quic_port': port}
            plain = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            wait_until(lambda: plain.open(cls.settings['relay_url'] + 'healthz', timeout=.3).status == 200)
        result = subprocess.run([str(SERVER), '--make-cert', '--names', 'localhost,127.0.0.1'],
            cwd=cls.directory, capture_output=True, creationflags=FLAGS, timeout=10)
        if result.returncode:
            raise AssertionError('Demo TLS fixture creation failed')
        certificate = cls.directory / '.state/tls/cert.pem'
        cls.context = ssl.create_default_context(cafile=str(certificate))
        cls.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
            urllib.request.HTTPSHandler(context=cls.context))
        port = free_port()
        cls.admin_base = f'https://127.0.0.1:{port}'
        cls.admin_token = uuid.uuid4().hex + uuid.uuid4().hex
        config = {'listen': f'127.0.0.1:{port}', 'token': cls.admin_token,
            'data': str(cls.directory / 'data'), 'tls_cert': str(certificate),
            'tls_key': str(certificate.with_name('key.pem')), 'p2p': cls.settings}
        config_path = cls.directory / 'server.json'
        config_path.write_text(json.dumps(config), encoding='utf-8')
        cls.start('demo', [str(SERVER), '--config', str(config_path), '--demo'])
        wait_until(lambda: cls.api(cls.admin_base, cls.admin_token, '/api/status').get('ready'))
        state = cls.api(cls.admin_base, cls.admin_token, '/api/local/p2p')
        assert state['enabled'] and state['status'] == 'listening' and state['server_id']
        cls.server_id = state['server_id']

    @classmethod
    def start(cls, name, arguments):
        log = (cls.directory / (name + '.log')).open('wb')
        cls.logs.append(log)
        process = subprocess.Popen(arguments, cwd=cls.directory,
            stdout=log, stderr=subprocess.STDOUT, creationflags=FLAGS)
        cls.processes.append(process)
        return process

    @classmethod
    def cleanup(cls):
        for process in reversed(cls.processes):
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for log in cls.logs:
            log.close()
        if destination := os.environ.get('WECHAT_P2P_TEST_LOG_DIR'):
            destination = Path(destination)
            destination.mkdir(parents=True, exist_ok=True)
            for log in cls.directory.glob('*.log'):
                shutil.copyfile(log, destination / log.name)
            (destination / 'transport-diagnostics.json').write_text(
                json.dumps(cls.diagnostics, indent=2), encoding='utf-8')
        cls.temporary.cleanup()

    @classmethod
    def api(cls, base, token, path, value=None, raw=None):
        data = json.dumps(value).encode() if value is not None else raw
        request = urllib.request.Request(base + path, data=data, headers={
            'Authorization': 'Bearer ' + token,
            'Content-Type': 'application/octet-stream' if raw is not None else 'application/json'})
        with cls.opener.open(request, timeout=40) as response:
            body = response.read()
            return json.loads(body) if response.headers.get_content_type() == 'application/json' else body

    def websocket(self, helper, token, after=0):
        # Only the safe WSS GET can be retried; no business POST is sent here.
        for attempt in range(2):
            timing = {'attempt': attempt + 1, 'paths': sorted(helper.paths)}
            self.diagnostics.append(timing)
            started = time.monotonic()

            def constructed(*args, **kwargs):
                timing['tls_completed_seconds'] = round(time.monotonic() - started, 3)
                return ClientConnection(*args, **kwargs)

            try:
                ws = connect(helper.base.replace('https://', 'wss://', 1) +
                    '/api/events?after=' + str(after), ssl=self.context,
                    additional_headers={'Authorization': 'Bearer ' + token},
                    proxy=None, open_timeout=20, create_connection=constructed)
                timing['upgrade_completed_seconds'] = round(time.monotonic() - started, 3)
                return ws
            except TimeoutError:
                timing['initial_timeout'] = True
                timing['elapsed_seconds'] = round(time.monotonic() - started, 3)
                self.assertIsNone(helper.process.poll())
                timing['same_helper_status_ready'] = self.api(helper.base, token, '/api/status')['ready']
                if attempt:
                    raise
                time.sleep(1)
        raise AssertionError('Unreachable')

    def exercise(self, relay_only):
        credential = self.api(self.admin_base, self.admin_token, '/api/local/clients', {
            'name': 'P2P isolated fixture', 'groups': ['demo-design'],
            'can_send': True, 'can_download': True})
        token = credential['token']
        helper = Helper(self.settings, self.server_id, self.directory, relay_only)
        self.addCleanup(helper.close)
        base = helper.base
        groups = self.api(base, token, '/api/groups')
        self.assertEqual([group['id'] for group in groups], ['demo-design'])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.api(base, self.admin_token, '/api/groups')
        self.assertEqual(caught.exception.code, 403)
        caught.exception.close()
        for path in ('/api/local/clients', '/api/local/forwarding', '/api/local/p2p', '/api/shutdown'):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.api(base, token, path)
            self.assertEqual(caught.exception.code, 404)
            caught.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.api(base, token, '/api/messages?group=demo-dev')
        self.assertEqual(caught.exception.code, 403)
        caught.exception.close()
        raw = os.urandom(8 * 1024 * 1024 + 13)
        before = len(list((self.directory / 'data/media').glob('*/*透明传输.bin')))
        media = self.api(base, token, '/api/media?group=demo-design&name=' +
            urllib.parse.quote('透明传输.bin'), raw=raw)
        self.assertEqual(media['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(self.api(base, token, '/api/media/' + media['id']), raw)
        self.assertEqual(len(list((self.directory / 'data/media').glob('*/*透明传输.bin'))), before + 1)
        ident = str(uuid.uuid4())
        cursor = 0
        with self.websocket(helper, token) as ws:
            while True:
                event = json.loads(ws.recv(timeout=15))
                cursor = max(cursor, event.get('seq', 0))
                if event.get('type') == 'synced':
                    break
        # A message accepted while WSS is disconnected must be replayed after reconnect.
        message = {'id': ident, 'group': 'demo-design', 'client_id': 'p2p-fixture',
            'sender': '本地演示', 'text': '@林小满 端到端协议测试', 'media': [media['id']],
            'mentions': [{'id': 'lin', 'name': '林小满'}]}
        self.api(base, token, '/api/messages', message)
        with self.websocket(helper, token, cursor) as ws:
            while True:
                event = json.loads(ws.recv(timeout=15))
                if event.get('type') == 'message' and event['message']['id'] == ident and event['message']['status'] == 'sent':
                    break
            self.api(base, token, '/api/messages', message)
            history = self.api(base, token, '/api/messages?group=demo-design')
            self.assertEqual(sum(row['id'] == ident for row in history), 1)
            self.api(self.admin_base, self.admin_token, '/api/local/clients/' + credential['client']['id'] + '/revoke', {})
            from websockets.exceptions import ConnectionClosed
            with self.assertRaises(ConnectionClosed):
                while True:
                    ws.recv(timeout=10)
        if relay_only:
            wait_until(lambda: 'relay' in helper.paths, 10)
            self.assertNotIn('direct', helper.paths)
        elif not os.environ.get('WECHAT_P2P_ENTRY'):
            wait_until(lambda: 'direct' in helper.paths, 15)
        self.assertIsNone(helper.process.poll())

    def test_normal_path_keeps_tls_permissions_files_events_and_receipts(self):
        self.exercise(False)

    def test_relay_only_path_keeps_the_same_application_contract(self):
        self.exercise(True)


if __name__ == '__main__':
    unittest.main(verbosity=2)

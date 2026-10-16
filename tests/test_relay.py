"""End-to-end transport tests. Only local demo/fixture adapters are used."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

from websockets.sync.client import connect

ROOT = Path(__file__).resolve().parents[1]
EXE = 'wechat-relay.exe' if os.name == 'nt' else 'wechat-relay'
RELEASE = ROOT / 'target' / 'release' / EXE
BINARY = Path(os.environ.get('WECHAT_RELAY_BINARY', RELEASE if RELEASE.exists() else ROOT / 'target' / 'debug' / EXE))


class RelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            cls.port = listener.getsockname()[1]
        cls.token = 'integration-test-' + uuid.uuid4().hex
        cls.base = f'http://127.0.0.1:{cls.port}'
        cls.process = subprocess.Popen([str(BINARY), '--demo', '--listen', f'127.0.0.1:{cls.port}', '--token', cls.token, '--data', cls.temp.name],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        for _ in range(100):
            try:
                urllib.request.urlopen(cls.base + '/health', timeout=0.1).close()
                return
            except (OSError, urllib.error.URLError):
                if cls.process.poll() is not None:
                    raise RuntimeError(cls.process.stderr.read().decode())
                time.sleep(0.05)
        raise RuntimeError('relay did not start')

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.wait(timeout=5)
        cls.process.stdout.close()
        cls.process.stderr.close()
        cls.temp.cleanup()

    def request(self, path, value=None, raw=None, token=None):
        data = json.dumps(value, ensure_ascii=False).encode() if value is not None else raw
        request = urllib.request.Request(self.base + path, data=data, headers={'Authorization': 'Bearer ' + (token or self.token),
            'Content-Type': 'application/octet-stream' if raw is not None else 'application/json'})
        with urllib.request.urlopen(request, timeout=5) as response:
            body = response.read()
            return json.loads(body) if response.headers.get('Content-Type', '').startswith('application/json') else body

    def message(self, **changes):
        value = {'id': str(uuid.uuid4()), 'group': 'demo-design', 'client_id': 'client-a', 'sender': '测试用户', 'text': '本机同步测试', 'media': [], 'mentions': []}
        value.update(changes)
        return value

    def websocket(self, after=0):
        ws = connect(f'ws://127.0.0.1:{self.port}/api/events?after={after}', additional_headers={'Authorization': 'Bearer ' + self.token}, proxy=None)
        while json.loads(ws.recv(timeout=5)).get('type') != 'synced':
            pass
        return ws

    def until_sent(self, ws, ident):
        events = []
        while True:
            event = json.loads(ws.recv(timeout=5))
            if event.get('type') == 'message' and event['message']['id'] == ident:
                events.append(event)
                if event['message']['status'] == 'sent':
                    return events

    def test_access_key_is_required_for_messages_and_files(self):
        for path in ('/api/groups', '/api/messages?group=demo-design', '/api/media/missing'):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.request(path, token='wrong')
            self.assertEqual(caught.exception.code, 401)

    def test_two_clients_receive_identical_confirmed_message(self):
        with self.websocket() as a, self.websocket() as b:
            value = self.message(text='@林小满 需要确认一下这份视频', mentions=[{'id': 'lin', 'name': '林小满'}])
            response = self.request('/api/messages', value)
            self.assertEqual(response['id'], value['id'])
            first, second = self.until_sent(a, value['id']), self.until_sent(b, value['id'])
            self.assertEqual(first, second)
            self.assertEqual(first[-1]['message']['mentions'][0]['id'], 'lin')

    def test_retry_is_idempotent_but_changed_content_is_rejected(self):
        value = self.message()
        first = self.request('/api/messages', value)
        second = self.request('/api/messages', value)
        self.assertEqual(first['seq'], second['seq'])
        history = self.request('/api/messages?group=demo-design')
        self.assertEqual(sum(m['id'] == value['id'] for m in history), 1)
        value['text'] = 'changed'
        with self.assertRaises(urllib.error.HTTPError):
            self.request('/api/messages', value)

    def test_mention_must_match_actual_member_and_visible_text(self):
        for mention, text in (({'id': 'unknown', 'name': '林小满'}, '@林小满'), ({'id': 'lin', 'name': '错误名字'}, '@错误名字'), ({'id': 'lin', 'name': '林小满'}, '没有提及')):
            with self.assertRaises(urllib.error.HTTPError):
                self.request('/api/messages', self.message(text=text, mentions=[mention]))

    def test_file_bytes_names_and_date_folders(self):
        raw = b'GIF89a' + os.urandom(2 * 1024 * 1024)
        media = self.request('/api/media?group=demo-design&name=' + urllib.parse.quote('../../原始动画.gif'), raw=raw)
        self.assertEqual(media['name'], '原始动画.gif')
        self.assertEqual(media['kind'], 'gif')
        self.assertNotIn('path', media)
        self.assertEqual(media['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(self.request('/api/media/' + media['id']), raw)
        stored = list((Path(self.temp.name) / 'media').glob('*/*原始动画.gif'))
        self.assertEqual(len(stored), 1)
        self.assertRegex(stored[0].parent.name, r'^\d{4}-\d{2}-\d{2}$')
        self.request('/api/messages', self.message(media=[media['id']]))
        history = self.request('/api/messages?group=demo-design')
        self.assertTrue(any(any(m['id'] == media['id'] for m in row['media']) for row in history))
        self.assertNotIn('path', json.dumps(history))

    def test_reconnect_replays_missed_events_in_order(self):
        first = self.message()
        with self.websocket() as ws:
            self.request('/api/messages', first)
            cursor = self.until_sent(ws, first['id'])[-1]['seq']
        second = self.message(text='断线期间的消息')
        self.request('/api/messages', second)
        with connect(f'ws://127.0.0.1:{self.port}/api/events?after={cursor}', additional_headers={'Authorization': 'Bearer ' + self.token}, proxy=None) as ws:
            seen = []
            while True:
                event = json.loads(ws.recv(timeout=5))
                if event.get('type') == 'message':
                    seen.append(event)
                if event.get('type') == 'synced':
                    break
            self.assertTrue(any(e['message']['id'] == second['id'] for e in seen))
            self.assertEqual([e['seq'] for e in seen], sorted(e['seq'] for e in seen))
            self.assertTrue(all(e['seq'] > cursor for e in seen))


if __name__ == '__main__':
    unittest.main(verbosity=2)

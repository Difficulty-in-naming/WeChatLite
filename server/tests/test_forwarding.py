"""Group selection is a server authorization boundary, including old event cursors."""
import json
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time
import unittest
import urllib.error
import urllib.request

import test_reliability as relay_helpers

BINARY = relay_helpers.BINARY


class ForwardingTests(unittest.TestCase):
    setUp = relay_helpers.ReliabilityTests.setUp
    tearDown = relay_helpers.ReliabilityTests.tearDown
    request = relay_helpers.ReliabilityTests.request
    websocket = relay_helpers.ReliabilityTests.websocket
    sync = relay_helpers.ReliabilityTests.sync
    message = relay_helpers.ReliabilityTests.message

    def start(self):
        config = {'listen': f'127.0.0.1:{self.port}', 'token': self.token, 'data': str(self.directory / 'state'),
            'bridge_program': sys.executable, 'bridge_args': [str(Path(__file__).with_name('fixture_forwarding.py')), str(self.directory)]}
        config_path = self.directory / 'server.json'
        config_path.write_text(json.dumps(config), encoding='utf-8')
        self.process = subprocess.Popen([str(BINARY), '--config', str(config_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.wait(lambda: self.request('/api/status').get('ready'))

    def stop(self):
        self.request('/api/shutdown', {})
        self.assertEqual(self.process.wait(timeout=6), 0)
        self.process.stdout.close()
        self.process.stderr.close()
        self.process = None

    def wait(self, check):
        for _ in range(100):
            try:
                result = check()
                if result:
                    return result
            except (OSError, urllib.error.URLError):
                if self.process.poll() is not None:
                    raise RuntimeError(self.process.stderr.read().decode())
            time.sleep(0.03)
        self.fail('fixture condition was not reached')

    def select(self, *groups):
        return self.request('/api/local/forwarding', {'selected': list(groups)})

    def rows(self, group):
        return self.request('/api/messages?group=' + group)

    def denied(self, path, value=None, raw=None, code=403):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.request(path, value=value, raw=raw)
        self.assertEqual(caught.exception.code, code)

    def stored(self, ident):
        with closing(sqlite3.connect(self.directory / 'state' / 'relay.sqlite')) as db:
            row = db.execute('SELECT body FROM messages WHERE id=?', (ident,)).fetchone()
        return json.loads(row[0]) if row else None

    def test_default_empty_local_management_validation_and_restart(self):
        self.start()
        self.assertEqual(self.request('/api/groups'), [])
        snapshot = self.request('/api/local/forwarding')
        self.assertEqual(snapshot['selected'], [])
        self.assertEqual({g['id'] for g in snapshot['groups']}, {'fixture-a', 'fixture-b'})
        for value in (None, {'selected': ['fixture-a']}):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.request('/api/local/forwarding', value, token='invalid')
            self.assertEqual(caught.exception.code, 401)
        self.denied('/api/local/forwarding', {'selected': ['unknown-group']}, code=400)
        self.assertEqual(self.request('/api/local/forwarding')['selected'], [])
        self.assertEqual(self.select('fixture-a', 'fixture-a')['selected'], ['fixture-a'])
        self.assertEqual([g['id'] for g in self.request('/api/groups')], ['fixture-a'])
        self.stop()
        self.start()
        self.assertEqual(json.loads((self.directory / 'initial-policy.json').read_text()), ['fixture-a'])
        self.assertEqual(self.request('/api/local/forwarding')['selected'], ['fixture-a'])
        self.select()
        self.stop()
        self.start()
        self.assertEqual(self.request('/api/groups'), [])
        self.assertEqual(json.loads((self.directory / 'initial-policy.json').read_text()), [])

    def test_history_members_media_old_cursors_and_live_events_cannot_bypass_selection(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        self.request('/api/groups/fixture-a/activate', {})
        a = self.wait(lambda: self.rows('fixture-a'))[0]
        b = self.wait(lambda: self.rows('fixture-b'))[0]
        uploaded = self.request('/api/media?group=fixture-a&name=owned.txt', raw=b'owned-by-a')
        self.denied('/api/media?name=missing-owner.txt', raw=b'data', code=400)
        self.denied('/api/messages', self.message(group='fixture-b', media=[uploaded['id']]), code=400)
        outgoing_b = self.message(group='fixture-b', text='before deselection')
        self.request('/api/messages', outgoing_b)
        self.wait(lambda: self.stored(outgoing_b['id'])['status'] == 'sent')
        self.select('fixture-a')
        for path, value in [('/api/messages?group=fixture-b', None), ('/api/groups/fixture-b/members', None),
                ('/api/groups/fixture-b/activate', {}), ('/api/messages', outgoing_b),
                ('/api/media/' + b['media'][0]['id'], None)]:
            self.denied(path, value)
        self.denied('/api/media?group=fixture-b&name=denied.txt', raw=b'data')
        with self.websocket(0) as ws:
            events = self.sync(ws)
            for event in events:
                if event.get('type') == 'message':
                    self.assertEqual(event['message']['group'], 'fixture-a')
            self.assertNotIn('Private body fixture-b', json.dumps(events))
            self.assertNotIn(b['media'][0]['id'], json.dumps(events))
        with self.websocket(-1) as ws:
            self.sync(ws)
            self.request('/api/groups/fixture-a/activate', {})
            self.wait(lambda: len(self.rows('fixture-a')) >= 2)
            barrier = self.message(group='fixture-a', text='event barrier')
            self.request('/api/messages', barrier)
            while True:
                event = json.loads(ws.recv(timeout=5))
                if event.get('type') == 'message':
                    self.assertEqual(event['message']['group'], 'fixture-a')
                    if event['message']['id'] == barrier['id'] and event['message']['status'] == 'sent':
                        break
        self.assertIsNone(self.stored('fixture-b-2'))
        with closing(sqlite3.connect(self.directory / 'state' / 'relay.sqlite')) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM media_owners WHERE group_id='fixture-b'").fetchone()[0], 1)
        self.select('fixture-b')
        self.denied('/api/media/' + a['media'][0]['id'])
        self.denied('/api/media/' + uploaded['id'])
        self.assertTrue(any(row['id'] == b['id'] for row in self.rows('fixture-b')))

    def test_deselection_cancels_queued_send_and_confirms_submitted_send_without_retry(self):
        self.start()
        self.select('fixture-a')
        first = self.message(group='fixture-a', text='hold until revoked')
        second = self.message(group='fixture-a', text='must never submit')
        self.request('/api/messages', first)
        self.wait(lambda: self.stored(first['id'])['status'] == 'sending')
        self.request('/api/messages', second)
        self.assertEqual(self.stored(second['id'])['status'], 'queued')
        self.select()
        self.wait(lambda: self.stored(first['id'])['status'] == 'sent')
        self.assertEqual(self.stored(second['id'])['status'], 'failed')
        self.denied('/api/messages', first)
        self.select('fixture-a')
        self.assertEqual(self.request('/api/messages', first)['status'], 'sent')
        commands = [json.loads(line) for line in (self.directory / 'commands.jsonl').read_text().splitlines()]
        self.assertEqual([c['message']['id'] for c in commands if c['op'] == 'send'], [first['id']])


if __name__ == '__main__':
    unittest.main(verbosity=2)

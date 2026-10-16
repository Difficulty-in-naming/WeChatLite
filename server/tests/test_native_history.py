"""Backward history is pulled on demand; cache exhaustion is not native EOF."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import urllib.error

import test_client_access as access
import test_reliability as helpers


class NativeHistoryTests(unittest.TestCase):
    setUp = access.ClientAccessTests.setUp
    tearDown = access.ClientAccessTests.tearDown
    stop = access.ClientAccessTests.stop
    request = access.ClientAccessTests.request
    wait = access.ClientAccessTests.wait
    select = access.ClientAccessTests.select
    rows = access.ClientAccessTests.rows
    key = access.ClientAccessTests.key
    update = access.ClientAccessTests.update
    deny = access.ClientAccessTests.deny
    websocket = helpers.ReliabilityTests.websocket
    sync = helpers.ReliabilityTests.sync

    def start(self):
        config = {'listen': f'127.0.0.1:{self.port}', 'token': self.token, 'data': str(self.directory / 'state'),
            'bridge_program': sys.executable, 'bridge_args': [str(Path(__file__).with_name('fixture_history.py')), str(self.directory)]}
        path = self.directory / 'server.json'
        path.write_text(json.dumps(config), encoding='utf-8')
        self.process = subprocess.Popen([str(helpers.BINARY), '--config', str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.wait(lambda: self.request('/api/status').get('ready'))

    def history(self, before=0, token=None, group='fixture-a'):
        return self.request(f'/api/history?group={group}&before={before}', token=token)

    def native_commands(self):
        path = self.directory / 'commands.jsonl'
        return [value for line in path.read_text().splitlines() if (value := json.loads(line))['op'] == 'history']

    def test_pages_beyond_initial_40_are_ordered_deduplicated_and_end_only_after_native_eof(self):
        self.start()
        self.select('fixture-a')
        self.request('/api/groups/fixture-a/activate', {})
        self.wait(lambda: len(self.rows('fixture-a')) == 40)
        before = 0
        found = {}
        for _ in range(8):
            response = self.history(before)
            self.assertFalse(response['pending'])
            page = response['messages']
            self.assertEqual([(row['timestamp'], row['seq']) for row in page], sorted((row['timestamp'], row['seq']) for row in page))
            found.update({row['id']: row for row in page})
            if page:
                oldest = min(found.values(), key=lambda row: (row['timestamp'], row['seq']))
                before = oldest['seq']
            if not response['has_more']:
                break
        else:
            self.fail('native EOF was not reached')
        self.assertEqual(len(found), 137)
        self.assertEqual(len(self.native_commands()), 3)
        self.assertFalse(self.history(before)['has_more'])
        self.assertTrue(all(row['id'] in found for row in self.history(before)['messages']))
        self.assertEqual(len(self.native_commands()), 3)
        self.stop()
        self.start()
        self.assertFalse(self.history(before)['has_more'])
        self.assertEqual(len(self.native_commands()), 3)

    def test_timeout_is_pending_and_same_cursor_requests_share_one_native_task(self):
        self.start()
        self.select('fixture-a')
        hold = self.directory / 'hold-history'
        hold.touch()
        with ThreadPoolExecutor(max_workers=2) as workers:
            a = workers.submit(self.history)
            b = workers.submit(self.history)
            for result in (a.result(timeout=6), b.result(timeout=6)):
                self.assertEqual(result, {'messages': [], 'has_more': True, 'pending': True})
        self.assertEqual(len(self.native_commands()), 1)
        hold.unlink()
        self.request('/api/groups/fixture-a/activate', {})
        self.wait(lambda: len(self.rows('fixture-a')) == 60)
        self.assertFalse(self.history()['pending'])
        self.assertEqual(len(self.native_commands()), 1)

    def test_group_and_before_validation_read_only_client_and_mid_request_revocation(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        reader = self.key(groups=['fixture-a'])
        token = reader['token']
        self.deny(token, '/api/history?group=fixture-b')
        self.deny(token, '/api/history?group=fixture-a&before=-1', code=400)
        self.deny(token, '/api/history?group=fixture-a&before=999999', code=400)
        self.deny(token, '/api/history?group=fixture-a&search=text', code=400)
        hold = self.directory / 'hold-history'
        hold.touch()
        with ThreadPoolExecutor(max_workers=1) as workers:
            pending = workers.submit(self.history, 0, token)
            self.wait(lambda: self.native_commands())
            self.update(reader, groups=[])
            hold.unlink()
            self.request('/api/groups/fixture-b/activate', {})
            with self.assertRaises(urllib.error.HTTPError) as caught:
                pending.result(timeout=6)
            self.assertEqual(caught.exception.code, 403)
        self.update(reader, groups=['fixture-a'])
        self.assertTrue(self.history(token=token)['messages'])
        foreign = self.wait(lambda:self.rows('fixture-b'))[0]['seq']
        self.deny(token, f'/api/history?group=fixture-a&before={foreign}', code=400)

    def test_native_error_is_retryable_failure_and_does_not_persist_false_eof(self):
        self.start()
        self.select('fixture-a')
        error = self.directory / 'history-error'
        error.touch()
        self.deny(self.token, '/api/history?group=fixture-a', code=503)
        error.unlink()
        self.assertTrue(self.history()['messages'])
        self.assertEqual(len(self.native_commands()), 2)

    def test_history_events_remain_marked_in_live_broadcast_and_durable_replay(self):
        self.start()
        self.select('fixture-a')
        with self.websocket(-1) as socket:
            self.sync(socket)
            response = self.history()
            self.assertEqual(len(response['messages']), 60)
            messages = []
            while len(messages) < 60:
                event = json.loads(socket.recv(timeout=5))
                if event['type'] == 'message':
                    messages.append(event)
            self.assertTrue(all(event.get('historical') is True for event in messages))
        with self.websocket(0) as socket:
            replay = [event for event in self.sync(socket) if event['type'] == 'message']
            self.assertEqual(len(replay), 60)
            self.assertTrue(all(event.get('historical') is True for event in replay))


if __name__ == '__main__':
    unittest.main(verbosity=2)

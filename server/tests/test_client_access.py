"""Per-key authorization against an offline adapter. Never connects to WeChat."""
from contextlib import closing
import hashlib
import json
import sqlite3
import unittest
import urllib.error
import urllib.request

from websockets.exceptions import ConnectionClosed, InvalidStatus
from websockets.sync.client import connect

import test_forwarding as fixtures


class ClientAccessTests(unittest.TestCase):
    setUp = fixtures.ForwardingTests.setUp
    tearDown = fixtures.ForwardingTests.tearDown
    start = fixtures.ForwardingTests.start
    stop = fixtures.ForwardingTests.stop
    wait = fixtures.ForwardingTests.wait
    request = fixtures.ForwardingTests.request
    sync = fixtures.ForwardingTests.sync
    message = fixtures.ForwardingTests.message
    select = fixtures.ForwardingTests.select
    rows = fixtures.ForwardingTests.rows
    stored = fixtures.ForwardingTests.stored

    def key(self, name='Independent client', **changes):
        return self.request('/api/local/clients', {'name': name, **changes})

    def update(self, key, **changes):
        return self.request('/api/local/clients/' + key['client']['id'], changes)['client']

    def deny(self, token, path, value=None, raw=None, code=403):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.request(path, value=value, raw=raw, token=token)
        self.assertEqual(caught.exception.code, code)

    def ws(self, token, after=0):
        return connect(f'ws://127.0.0.1:{self.port}/api/events?after={after}',
            additional_headers={'Authorization': 'Bearer ' + token}, proxy=None)

    def bytes(self, token, media):
        request = urllib.request.Request(self.base + '/api/media/' + media,
            headers={'Authorization': 'Bearer ' + token})
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.read()

    def test_create_once_hash_storage_defaults_and_restart_preserve_global_policy(self):
        self.start()
        self.select('fixture-a')
        created = self.key()
        token = created['token']
        client = created['client']
        self.assertEqual(client['groups'], [])
        self.assertFalse(client['can_send'])
        self.assertFalse(client['can_download'])
        self.assertTrue(client['enabled'])
        self.assertEqual(self.request('/api/groups', token=token), [])
        status = self.request('/api/status', token=token)
        self.assertEqual(status['authenticated_client']['id'], client['id'])
        self.assertFalse(status['authenticated_client']['admin'])
        self.assertEqual(status['permissions'], {'groups': [], 'can_send': False, 'can_download': False})
        listing = self.request('/api/local/clients')
        self.assertEqual(listing['clients'], [client])
        self.assertNotIn(token, json.dumps(listing))
        self.assertNotIn('token_hash', json.dumps(listing))
        with closing(sqlite3.connect(self.directory / 'state' / 'relay.sqlite')) as db:
            row = db.execute('SELECT token_hash,body FROM access_clients WHERE id=?', (client['id'],)).fetchone()
            self.assertEqual(row[0], hashlib.sha256(token.encode()).hexdigest())
            self.assertNotIn(token, row[1])
        self.deny(self.token, '/api/local/clients', {'name': 'Bad group', 'groups': ['missing']}, code=400)
        self.assertEqual(len(self.request('/api/local/clients')['clients']), 1)
        self.update(created, name='Renamed', groups=['fixture-a', 'fixture-b'], can_download=True)
        self.assertEqual([g['id'] for g in self.request('/api/groups', token=token)], ['fixture-a'])
        self.stop()
        self.start()
        self.assertEqual(self.request('/api/local/forwarding')['selected'], ['fixture-a'])
        restored = self.request('/api/local/clients')['clients'][0]
        self.assertEqual(restored['name'], 'Renamed')
        self.assertEqual(restored['groups'], ['fixture-a', 'fixture-b'])
        self.assertEqual([g['id'] for g in self.request('/api/groups', token=token)], ['fixture-a'])

    def test_client_key_never_gains_local_administrator_and_spoofed_headers_do_not_help(self):
        self.start()
        client = self.key(groups=['fixture-a'], can_send=True, can_download=True)
        token = client['token']
        for path, value in [('/api/local/forwarding', None), ('/api/local/forwarding', {'selected': ['fixture-a']}),
                ('/api/local/clients', None), ('/api/local/clients', {'name': 'Escalation'}),
                ('/api/local/clients/' + client['client']['id'], {'can_send': True}),
                ('/api/local/clients/' + client['client']['id'] + '/revoke', {}), ('/api/shutdown', {})]:
            self.deny(token, path, value)
        request = urllib.request.Request(self.base + '/api/local/clients', headers={
            'Authorization': 'Bearer ' + token, 'X-Forwarded-For': '127.0.0.1', 'X-Admin': 'true'})
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=5)
        self.assertEqual(caught.exception.code, 403)
        self.assertTrue(self.request('/api/status')['ready'])

    def test_group_send_download_quote_history_and_replay_boundaries(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        self.request('/api/groups/fixture-a/activate', {})
        a = self.wait(lambda: self.rows('fixture-a'))[0]
        b = self.wait(lambda: self.rows('fixture-b'))[0]
        reader = self.key(groups=['fixture-a'])
        token = reader['token']
        self.assertEqual([g['id'] for g in self.request('/api/groups', token=token)], ['fixture-a'])
        self.assertEqual(self.request('/api/messages?group=fixture-a', token=token)[0]['id'], a['id'])
        self.assertTrue(self.request('/api/groups/fixture-a/members', token=token))
        self.assertEqual(self.request('/api/messages/' + a['id'] + '?group=fixture-a', token=token)['id'], a['id'])
        for path, value in [('/api/messages?group=fixture-b', None), ('/api/groups/fixture-b/members', None),
                ('/api/groups/fixture-b/activate', {}), ('/api/messages/' + b['id'] + '?group=fixture-b', None),
                ('/api/messages', self.message(group='fixture-b')), ('/api/media/' + b['media'][0]['id'], None)]:
            self.deny(token, path, value)
        self.deny(token, '/api/media/' + a['media'][0]['id'])
        self.deny(token, '/api/media?group=fixture-a&name=blocked.txt', raw=b'blocked')
        self.deny(token, '/api/messages', self.message(group='fixture-a', client_id='local-admin'))
        original = self.message(group='fixture-a', text='Shared outgoing original')
        self.request('/api/messages', original)
        self.wait(lambda: self.stored(original['id'])['status'] == 'sent')
        self.deny(token, '/api/messages/' + original['id'] + '/recall', {})
        with self.ws(token) as socket:
            events = self.sync(socket)
            self.assertNotIn('fixture-b', json.dumps(events))
            self.assertTrue(all(event['message']['group'] == 'fixture-a' for event in events if event['type'] == 'message'))
            self.assertNotIn('access_client', json.dumps(events))
        self.update(reader, can_send=True, can_download=True)
        self.assertEqual(self.bytes(token, a['media'][0]['id']), b'private bytes fixture-a')
        self.deny(token, '/api/media/' + b['media'][0]['id'])
        self.deny(token, '/api/messages', self.message(group='fixture-a', quote={'id': b['id']}), code=400)
        uploaded = self.request('/api/media?group=fixture-a&name=allowed.txt', raw=b'client attachment', token=token)
        outgoing = self.message(group='fixture-a', text='Independent send', media=[uploaded['id']])
        submitted = self.request('/api/messages', outgoing, token=token)
        self.assertNotIn('access_client', submitted)
        self.wait(lambda: self.stored(outgoing['id'])['status'] == 'sent')
        self.assertEqual(self.stored(outgoing['id'])['access_client'], reader['client']['id'])
        self.request('/api/messages/' + outgoing['id'] + '/recall', {}, token=token)
        self.wait(lambda: self.stored(outgoing['id'])['status'] == 'recalled')
        self.select('fixture-b')
        self.assertEqual(self.request('/api/groups', token=token), [])
        self.deny(token, '/api/messages?group=fixture-a')
        self.deny(token, '/api/media/' + uploaded['id'])

    def test_permission_update_closes_only_changed_session_and_reconnect_filters_old_and_live_events(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        first = self.key(name='First', groups=['fixture-a', 'fixture-b'])
        other = self.key(name='Other', groups=['fixture-a'])
        self.request('/api/groups/fixture-a/activate', {})
        self.wait(lambda: self.rows('fixture-b'))
        with self.ws(first['token'], -1) as socket, self.ws(other['token'], -1) as unaffected:
            self.sync(socket)
            self.sync(unaffected)
            self.assertEqual(self.request('/api/status')['online_clients'], 2)
            with self.ws(first['token'], -1) as duplicate:
                self.sync(duplicate)
                self.assertEqual(self.request('/api/status')['online_clients'], 2)
            self.update(first, groups=['fixture-a'])
            with self.assertRaises(ConnectionClosed):
                socket.recv(timeout=5)
            self.wait(lambda: self.request('/api/status')['online_clients'] == 1)
            self.request('/api/groups/fixture-a/activate', {}, token=other['token'])
            while True:
                event = json.loads(unaffected.recv(timeout=5))
                if event['type'] == 'message':
                    self.assertEqual(event['message']['group'], 'fixture-a')
                    break
        with self.ws(first['token'], 0) as socket:
            events = self.sync(socket)
            self.assertNotIn('fixture-b', json.dumps(events))
            self.request('/api/groups/fixture-a/activate', {})
            while True:
                event = json.loads(socket.recv(timeout=5))
                if event['type'] == 'message':
                    self.assertEqual(event['message']['group'], 'fixture-a')
                    break
            self.update(first, enabled=False)
            with self.assertRaises(ConnectionClosed):
                socket.recv(timeout=5)
        self.deny(first['token'], '/api/groups', code=401)
        with self.assertRaises(InvalidStatus) as caught:
            self.ws(first['token'])
        self.assertEqual(caught.exception.response.status_code, 401)
        self.update(first, enabled=True)
        self.assertTrue(self.request('/api/groups', token=first['token']))
        self.request('/api/local/clients/' + first['client']['id'] + '/revoke', {})
        self.deny(first['token'], '/api/status', code=401)
        self.deny(self.token, '/api/local/clients/' + first['client']['id'], {'enabled': True}, code=400)
        self.stop()
        self.start()
        self.deny(first['token'], '/api/groups', code=401)
        self.assertTrue(self.request('/api/groups', token=other['token']))

    def test_revocation_cancels_only_owned_queued_work_and_never_retries_submitted_send(self):
        self.start()
        self.select('fixture-a')
        owner = self.key(groups=['fixture-a'], can_send=True)
        other = self.key(groups=['fixture-a'], can_send=True)
        first = self.message(group='fixture-a', text='hold pending acknowledgement')
        queued = self.message(group='fixture-a', text='must not submit')
        untouched = self.message(group='fixture-a', text='other client queue')
        self.request('/api/messages', first, token=owner['token'])
        self.wait(lambda: self.stored(first['id'])['status'] == 'sending')
        self.request('/api/messages', queued, token=owner['token'])
        self.request('/api/messages', untouched, token=other['token'])
        self.deny(other['token'], '/api/messages', first, code=400)
        self.update(owner, can_send=False)
        self.assertEqual(self.stored(queued['id'])['status'], 'failed')
        self.assertEqual(self.stored(untouched['id'])['status'], 'queued')
        self.assertEqual(self.stored(first['id'])['status'], 'sending')
        self.deny(owner['token'], '/api/messages', self.message(group='fixture-a', client_id=other['client']['id']))
        # Fixture releases the already submitted ACK when global forwarding stops.
        self.select()
        self.wait(lambda: self.stored(first['id'])['status'] == 'sent')
        self.select('fixture-a')
        self.update(owner, can_send=True)
        self.assertEqual(self.request('/api/messages', first, token=owner['token'])['status'], 'sent')
        self.stop()
        self.start()
        commands = [json.loads(line) for line in (self.directory / 'commands.jsonl').read_text().splitlines()]
        self.assertEqual([command['message']['id'] for command in commands if command['op'] == 'send'], [first['id']])


if __name__ == '__main__':
    unittest.main(verbosity=2)

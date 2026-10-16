"""Quote and recall are tested only with an offline adapter fixture."""
from contextlib import closing
import json
import sqlite3
import time
import unittest
import urllib.error

import test_forwarding as fixtures


class MessageActionsTests(unittest.TestCase):
    setUp = fixtures.ForwardingTests.setUp
    tearDown = fixtures.ForwardingTests.tearDown
    start = fixtures.ForwardingTests.start
    stop = fixtures.ForwardingTests.stop
    wait = fixtures.ForwardingTests.wait
    request = fixtures.ForwardingTests.request
    websocket = fixtures.ForwardingTests.websocket
    sync = fixtures.ForwardingTests.sync
    message = fixtures.ForwardingTests.message
    select = fixtures.ForwardingTests.select
    rows = fixtures.ForwardingTests.rows
    denied = fixtures.ForwardingTests.denied
    stored = fixtures.ForwardingTests.stored

    def confirmed(self, text='Fixture original', **changes):
        value = self.message(group='fixture-a', text=text, **changes)
        self.request('/api/messages', value)
        self.wait(lambda: self.stored(value['id'])['status'] == 'sent')
        return self.stored(value['id'])

    def change(self, ident, **changes):
        value = self.stored(ident)
        value.update(changes)
        with closing(sqlite3.connect(self.directory / 'state' / 'relay.sqlite')) as db:
            db.execute('UPDATE messages SET body=?,status=? WHERE id=?', (json.dumps(value), value['status'], ident))
            db.commit()

    def test_locate_requires_current_group_selection_and_never_returns_recalled_content(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        original = self.confirmed(text='Locate original')
        path = '/api/messages/' + original['id']
        located = self.request(path + '?group=fixture-a')
        self.assertEqual(located['id'], original['id'])
        self.assertGreater(located['seq'], 0)
        self.assertEqual(located['text'], 'Locate original')
        self.denied(path + '?group=fixture-b', code=404)
        self.select('fixture-b')
        self.denied(path + '?group=fixture-a', code=403)
        self.select('fixture-a')
        self.request(path + '/recall', {})
        self.wait(lambda: self.stored(original['id'])['status'] == 'recalled')
        self.denied(path + '?group=fixture-a', code=410)

    def test_quote_canonical_snapshot_cross_group_forgery_and_withdrawal(self):
        self.start()
        self.select('fixture-a', 'fixture-b')
        original = self.confirmed(text='Original quote body')
        for quote in ({'id': ''}, {'id': 'missing'}, {'id': original['id'], 'sender': 'Forged name'},
                {'id': original['id'], 'text': 'Forged text'}, {'id': original['id'], 'unavailable': True}):
            self.denied('/api/messages', self.message(group='fixture-a', quote=quote), code=400)
        self.denied('/api/messages', self.message(group='fixture-b', quote={'id': original['id']}), code=400)
        reply = self.confirmed(text='Reply', quote={'id': original['id']})
        self.assertEqual(reply['quote'], {'id': original['id'], 'sender': original['sender'], 'text': original['text'], 'unavailable': False})
        self.assertEqual(len(original['source_ids']), 2)
        pending = self.request('/api/messages/' + original['id'] + '/recall', {})
        self.assertEqual(pending['recall_status'], 'pending')
        self.wait(lambda: self.stored(original['id'])['status'] == 'recalled')
        self.assertEqual(self.stored(original['id'])['text'], '')
        quote = self.stored(reply['id'])['quote']
        self.assertTrue(quote['unavailable'])
        self.assertEqual(quote['text'], '')
        self.denied('/api/messages', self.message(group='fixture-a', quote={'id': original['id']}), code=400)
        with self.websocket(0) as ws:
            self.assertNotIn('Original quote body', json.dumps(self.sync(ws)))
        self.assertEqual(self.request('/api/messages/' + original['id'] + '/recall', {})['status'], 'recalled')

    def test_recall_requires_token_selection_owner_confirmation_and_time_window(self):
        self.start()
        self.select('fixture-a')
        self.request('/api/groups/fixture-a/activate', {})
        incoming = self.wait(lambda: self.rows('fixture-a'))[0]
        self.denied('/api/messages/' + incoming['id'] + '/recall', {}, code=400)
        for timestamp in (int(time.time() * 1000) - 181000, int(time.time() * 1000) + 60000):
            original = self.confirmed()
            self.change(original['id'], timestamp=timestamp)
            self.denied('/api/messages/' + original['id'] + '/recall', {}, code=400)
        unconfirmed = self.confirmed()
        self.change(unconfirmed['id'], source_id='', source_ids=[])
        self.denied('/api/messages/' + unconfirmed['id'] + '/recall', {}, code=400)
        original = self.confirmed()
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.request('/api/messages/' + original['id'] + '/recall', {}, token='invalid')
        self.assertEqual(caught.exception.code, 401)
        self.select()
        self.denied('/api/messages/' + original['id'] + '/recall', {})

    def test_native_quote_and_reply_have_independent_utf8_capacity_before_enqueue(self):
        self.start()
        self.select('fixture-a')
        original = self.confirmed(text='字' * 7900)
        oversized = self.message(group='fixture-a', text='x' * 24001, quote={'id': original['id']})
        self.denied('/api/messages', oversized, code=400)
        self.assertIsNone(self.stored(oversized['id']))
        accepted = self.confirmed(text='x' * 24000, quote={'id': original['id']})
        self.assertEqual(len(accepted['text'].encode('utf-8')), 24000)
        self.assertEqual(accepted['quote']['text'], original['text'])

    def test_pending_recall_survives_restart_as_check_and_never_reissues_action(self):
        self.start()
        self.select('fixture-a')
        original = self.confirmed(text='recall hold until restart')
        endpoint = '/api/messages/' + original['id'] + '/recall'
        self.request(endpoint, {})
        self.request(endpoint, {})
        self.wait(lambda: any(json.loads(line)['op'] == 'recall' for line in (self.directory / 'commands.jsonl').read_text().splitlines()))
        self.stop()
        self.assertEqual(self.stored(original['id'])['recall_status'], 'pending')
        self.start()
        self.wait(lambda: self.stored(original['id'])['status'] == 'recalled')
        commands = [json.loads(line)['op'] for line in (self.directory / 'commands.jsonl').read_text().splitlines()]
        self.assertEqual(commands.count('recall'), 1)
        self.assertEqual(commands.count('recall_check'), 1)

    def test_failed_or_uncertain_confirmation_preserves_body_and_does_not_retry(self):
        self.start()
        self.select('fixture-a')
        for status in ('failed', 'uncertain'):
            original = self.confirmed(text='recall ' + ('fail' if status == 'failed' else status))
            endpoint = '/api/messages/' + original['id'] + '/recall'
            self.request(endpoint, {})
            self.wait(lambda: self.stored(original['id']).get('recall_status') == status)
            self.assertEqual(self.stored(original['id'])['text'], original['text'])
            self.assertEqual(self.request(endpoint, {})['recall_status'], status)
        commands = [json.loads(line)['op'] for line in (self.directory / 'commands.jsonl').read_text().splitlines()]
        self.assertEqual(commands.count('recall'), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)

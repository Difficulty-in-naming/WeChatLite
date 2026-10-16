"""Offline adapter regression tests. Native sends are always fake; no live WeChat access."""
import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.error
import uuid
import wave
import weakref
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wechat_adapter', ROOT / 'adapters/windows/bridge_main.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
BACKEND = adapter.load_backend()
from memory_db import MemoryWeChatDB, MemoryConnection

GROUP = '123456@chatroom'


def row(sequence=10, local_id=1, shard='message__message_0.db', **changes):
    value = dict(local_id=local_id, sort_seq=sequence, shard=shard, content='你好', type='文本',
        sender_id=2, sender_username='', create_time=1_760_000_000, server_id=sequence + local_id,
        source='')
    value.update(changes)
    return value


class FakeDB:
    wxid = 'wxid_self'

    def __init__(self):
        self.rows = []
        self.groups_rows = {}
        self.avatars = {}
        self.avatar_reads = {}

    def get_avatar(self, ident):
        self.avatar_reads[ident] = self.avatar_reads.get(ident, 0) + 1
        return self.avatars.get(ident)

    def get_group_members(self, group):
        return [dict(username='member-a', nick_name='Nick', remark=''),
                dict(username='member-b', nick_name='Nick', remark='')]

    def get_display_name(self, group, ident):
        return {'member-a': 'Nick', 'member-b': 'Nick', 'wxid_self': '我'}.get(ident, ident)

    def get_messages(self, group, limit=1):
        return sorted(self.groups_rows.get(group, self.rows), key=lambda item: item['sort_seq'], reverse=True)[:limit]

    def get_new_messages(self, group, since_seq=0, limit=200):
        return [item for item in self.groups_rows.get(group, self.rows) if item['sort_seq'] > since_seq][:limit]

    def get_message_row(self, group, local_id, shard):
        return next((item for item in self.groups_rows.get(group, self.rows) if item['local_id'] == local_id and item['shard'] == shard), None)

    def reload_message(self, group, locator):
        message = self.get_message_row(group, locator['local_id'], locator['shard'])
        if message is None or message['sort_seq'] != locator['sort_seq']:
            raise RuntimeError('fixture source not found')
        return message

    def get_new_message_page(self, group, position, limit=200):
        key = lambda item: (item['sort_seq'], item['shard'], item['local_id'])
        return [item for item in sorted(self.groups_rows.get(group, self.rows), key=key) if key(item) > key(position)][:limit]


class FakeNative:
    pid = 123

    def __init__(self, db):
        self.db, self.calls, self.mode = db, [], 'echo'

    def request(self, path):
        assert path == '/health'
        return {'pid': self.pid, 'capabilities': ['gif']}

    def echo(self, content, kind='文本', source=''):
        self.db.rows.append(row(sequence=100 + len(self.calls), local_id=1 + len(self.calls),
            content=content, type=kind, source=source))

    def send(self, group, text, key, atlist=''):
        self.calls.append((group, text, key, atlist))
        if self.mode == 'rejected':
            raise urllib.error.HTTPError('http://127.0.0.1', 400, 'invalid', {}, None)
        if self.mode == 'lost':
            raise OSError('reply lost after submission')
        if self.mode == 'echo':
            self.echo(text, source='<msgsource><atuserlist>' + atlist + '</atuserlist></msgsource>')
        return {'status': 'queued', 'ret': 0}

    def send_media(self, group, path, kind, key):
        data = Path(path).read_bytes()
        self.calls.append((group, Path(path).name, kind, key, data))
        digest = hashlib.md5(data).hexdigest()
        if self.mode == 'lost':
            raise OSError('reply lost after submission')
        if kind == 'gif':
            self.echo(f'<msg><emoji md5="{digest}" len="{len(data)}"/></msg>', '动画表情')
        elif kind == 'image':
            self.echo(f'<msg><img md5="{digest}"/></msg>', '图片')
        elif Path(path).suffix == '.mp4':
            self.echo(f'<msg><videomsg md5="{digest}" length="{len(data)}"/></msg>', '视频')
        else:
            self.echo(f'<msg><appmsg><type>6</type><title>{Path(path).name}</title><md5>{digest}</md5>'
                f'<totallen>{len(data)}</totallen></appmsg></msg>', '文件')
        return {'status': 'queued', 'ret': 0}


class AdapterTests(unittest.TestCase):
    def setUp(self):
        adapter.avatar_data.cache_clear()
        self.addCleanup(adapter.avatar_data.cache_clear)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name)
        self.directory = self.data / 'adapter'
        self.directory.mkdir()
        self.receipts = adapter.Receipts(self.directory / 'receipts.sqlite')
        self.addCleanup(self.receipts.db.close)
        self.access = BACKEND.WeChatAccess.__new__(BACKEND.WeChatAccess)
        self.access.directory = self.directory / 'wechat'
        self.access.db = FakeDB()
        self.access.native = FakeNative(self.access.db)
        self.access.confirmation_slice = .005

    def message(self, **changes):
        value = {'id': str(uuid.uuid4()), 'group': GROUP, 'text': '你好', 'mentions': [], 'media': []}
        value.update(changes)
        return value

    def attachment(self, name='文档.txt', data=b'file contents', kind='file', folder='first'):
        path = self.data / 'media' / folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return dict(id=uuid.uuid4().hex, path=str(path), name=name, kind=kind,
            size=len(data), sha256=hashlib.sha256(data).hexdigest())

    def job(self, message):
        return adapter.OutgoingMessage(BACKEND, self.access, self.receipts, self.data,
            self.directory, {GROUP}, message)

    def run_main_fixture(self, commands=(), selected='[]', native_mode='echo', refreshed_catalog=None):
        other_group = '987654@chatroom'
        catalog = [{'id': GROUP, 'name': '本机甲群'}, {'id': other_group, 'name': '本机乙群'}]
        access = self.access
        access.db.account_dir = str(self.data / 'account')
        access.db.groups_rows = {GROUP: [row(sequence=10)], other_group: [row(sequence=20)]}
        reads = []
        original_read = access.db.get_messages
        access.db.get_messages = lambda group, limit=1: (reads.append(group), original_read(group, limit))[1]
        access.db.snapshot_scope = lambda: contextlib.nullcontext()
        access.db.release = lambda: None
        catalogs = [catalog, refreshed_catalog or catalog]
        access.groups = lambda: catalogs.pop(0) if len(catalogs) > 1 else catalogs[0]
        access.native = None
        connections = []
        instances = []
        class LocalNative(FakeNative):
            def __init__(native, directory, dll_path):
                super().__init__(access.db)
                native.mode = native_mode
                native.root = Path(directory)
                instances.append(native)
            def connect(native, groups, media_root, disable_older):
                connections.append(set(groups))
                folder = native.root / 'fixture-hash'
                folder.mkdir(parents=True, exist_ok=True)
                (folder / 'native.json').write_text(json.dumps({'groups': list(groups), 'token': 'fixture-token', 'port': 1234}), 'utf-8')
                return {'capabilities': ['text', 'mentions', 'image', 'gif', 'file', 'video_download']}
        backend = types.SimpleNamespace(**{**vars(BACKEND), 'WeChatAccess': lambda directory: access,
            'NativeClient': LocalNative, 'main_process': lambda: None})
        events = []
        allowlists_at_status = []
        def collect(event):
            events.append(event)
            if event['type'] == 'status':
                allowlists_at_status.append([json.loads(config.read_text('utf-8')).get('groups')
                    for config in (self.directory / 'native').glob('*/native.json')])
        environment = {'WECHAT_LITE_DATA': str(self.data), 'WECHAT_LITE_FORWARD_GROUPS': selected}
        with patch.dict(os.environ, environment), patch.object(adapter, 'load_backend', return_value=backend), \
            patch.object(adapter.sys, 'stdin', io.StringIO(''.join(json.dumps(command) + '\n' for command in commands))), \
            patch.object(adapter, 'emit', side_effect=collect):
            self.assertEqual(adapter.main(), 0)
        return types.SimpleNamespace(events=events, reads=reads, connections=connections,
            instances=instances, allowlists_at_status=allowlists_at_status)

    def test_lean_import_does_not_load_gui_or_opencv(self):
        self.assertNotIn('cv2', sys.modules)
        self.assertNotIn('uiautomation', sys.modules)
        self.assertNotIn('wechatauto.wx', sys.modules)
        self.assertIs(BACKEND, BACKEND)
        self.assertTrue(adapter.load_backend().WeChatAccess)

    def test_avatar_is_local_bounded_jpeg_and_shared_between_all_models(self):
        from PIL import Image
        image = Image.new('RGBA', (300, 200), (30, 100, 190, 200))
        original = io.BytesIO()
        image.save(original, format='PNG')
        content = original.getvalue()
        self.access.db.avatars = {GROUP: content, 'member-a': content}
        self.access.groups = lambda: [{'id': GROUP, 'name': '本机群'}]
        groups = adapter.group_list(self.access)
        members = adapter.member_list(self.access, GROUP)
        message = adapter.convert_message(BACKEND, self.access, GROUP,
            row(sender_id=9, sender_username='member-a', content='你好'))
        self.assertTrue(groups[0]['avatar'])
        self.assertEqual(groups[0]['avatar'], members[0]['avatar'])
        self.assertEqual(members[0]['avatar'], message['avatar'])
        self.assertEqual(members[1]['avatar'], '')
        with Image.open(io.BytesIO(base64.b64decode(message['avatar'], validate=True))) as thumbnail:
            self.assertEqual(thumbnail.format, 'JPEG')
            self.assertLessEqual(max(thumbnail.size), 64)
            self.assertEqual(thumbnail.mode, 'RGB')
        self.assertEqual(self.access.db.avatar_reads['member-a'], 1)
        self.assertEqual(self.access.db.avatars['member-a'], content)

    def test_bad_or_missing_avatars_are_cached_and_do_not_block_messages(self):
        self.access.db.avatars = {'member-a': b'incomplete image'}
        self.assertEqual(adapter.avatar_data(self.access.db, ''), '')
        for _ in range(3):
            self.assertEqual(adapter.avatar_data(self.access.db, 'member-a'), '')
            self.assertEqual(adapter.avatar_data(self.access.db, 'missing'), '')
        self.assertEqual(self.access.db.avatar_reads, {'member-a': 1, 'missing': 1})
        with patch.object(self.access.db, 'get_avatar', side_effect=OSError('busy local cache')):
            self.assertEqual(adapter.avatar_data(self.access.db, 'unavailable'), '')

    def test_avatar_lru_has_a_fixed_capacity(self):
        for index in range(600):
            self.assertEqual(adapter.avatar_data(self.access.db, 'member-' + str(index)), '')
        self.assertEqual(adapter.avatar_data.cache_info().maxsize, 512)
        self.assertEqual(adapter.avatar_data.cache_info().currsize, 512)
        adapter.avatar_data(self.access.db, 'member-0')
        self.assertEqual(self.access.db.avatar_reads['member-0'], 2)

    def test_timestamp_seconds_milliseconds_fraction_and_iso(self):
        self.assertEqual(adapter.timestamp_ms(1760000000), 1760000000000)
        self.assertEqual(adapter.timestamp_ms('1760000000000'), 1760000000000)
        self.assertEqual(adapter.timestamp_ms('1760000000.125'), 1760000000125)
        self.assertEqual(adapter.timestamp_ms('2025-10-09T08:53:20Z'), 1760000000000)

    def test_receive_member_identity_and_real_mention_ids(self):
        message = row(sender_id=7, sender_username='member-a', content='member-a:\n@Nick\u2005你好',
            source='<msgsource><atuserlist>member-b,member-b,notify@all</atuserlist></msgsource>')
        converted = adapter.convert_message(BACKEND, self.access, GROUP, message)
        self.assertEqual(converted['text'], '@Nick\u2005你好')
        self.assertEqual(converted['sender_id'], 'member-a')
        self.assertFalse(converted['outgoing'])
        self.assertEqual(converted['mentions'], [{'id': 'member-b', 'name': 'Nick'}, {'id': 'notify@all', 'name': '所有人'}])
        self.assertEqual(converted['id'], adapter.convert_message(BACKEND, self.access, GROUP, message)['id'])
        self.assertEqual(converted['timestamp'], 1760000000000)

    def test_unsupported_plain_card_is_readable_and_system_is_ignored(self):
        converted = adapter.convert_message(BACKEND, self.access, GROUP, row(type='未知消息', content='placeholder'))
        self.assertEqual(converted['text'], '[未知消息]')
        self.assertIsNone(adapter.convert_message(BACKEND, self.access, GROUP, row(type='系统消息')))

    def test_send_real_mention_uses_explicit_id_even_for_duplicate_names(self):
        message = self.message(text='@Nick\u2005你好', mentions=[{'id': 'member-b', 'name': 'Nick'}])
        ack = self.job(message).advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertEqual(self.access.native.calls[0][3], 'member-b')
        self.assertTrue(ack['source_id'].startswith('wx:' + GROUP + ':'))
        self.assertTrue(self.receipts.is_echo(ack['source_id']))
        repeated = self.job(message).advance()
        self.assertEqual(ack, repeated)
        self.assertEqual(len(self.access.native.calls), 1)

    def test_invalid_mentions_and_outside_paths_never_submit(self):
        for mention in ({'id': 'missing', 'name': 'Nick'}, {'id': 'member-a', 'name': 'Old name'}):
            with self.assertRaises(ValueError):
                self.job(self.message(text='@Nick\u2005你好', mentions=[mention]))
        media = self.attachment()
        media['path'] = str(self.data / 'receipts.sqlite')
        with self.assertRaises(ValueError):
            self.job(self.message(media=[media]))
        media = self.attachment()
        media['name'] = '../../escape.txt'
        with self.assertRaises(ValueError):
            self.job(self.message(media=[media]))
        self.assertFalse(self.access.native.calls)

    def test_all_attachment_hashes_are_checked_before_first_submission(self):
        valid = self.attachment()
        corrupt = self.attachment(folder='second')
        corrupt['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, '校验失败'):
            self.job(self.message(media=[valid, corrupt]))
        self.assertFalse(self.access.native.calls)

    def test_text_and_duplicate_named_files_have_distinct_parts_and_echoes(self):
        first = self.attachment(data=b'first')
        second = self.attachment(data=b'second', folder='second')
        job = self.job(self.message(media=[first, second]))
        self.assertIsNone(job.advance())
        self.assertIsNone(job.advance())
        ack = job.advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertEqual(len(ack['source_ids']), 3)
        self.assertTrue(all(self.receipts.is_echo(source) for source in ack['source_ids']))
        self.assertEqual(self.access.native.calls[1][-1], b'first')
        self.assertEqual(self.access.native.calls[2][-1], b'second')
        self.assertFalse(job.folder.exists())

    def test_mp4_uses_verified_native_video_echo(self):
        media = self.attachment(name='video.mp4', data=b'\x00\x00\x00\x18ftypmp42' + b'0' * 24, kind='video')
        ack = self.job(self.message(text='', media=[media])).advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertEqual(self.access.native.calls[0][2], 'file')

    def test_gif_uses_native_animation_channel_and_is_recognized_on_receive(self):
        from PIL import Image
        path = self.data / 'generated.gif'
        Image.new('RGB', (2, 2), 'red').save(path, save_all=True,
            append_images=[Image.new('RGB', (2, 2), 'blue')], duration=100, loop=0)
        media = self.attachment(name='animation.gif', data=path.read_bytes(), kind='gif')
        ack = self.job(self.message(text='', media=[media])).advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertEqual(self.access.native.calls[0][2], 'gif')
        self.access.emoji_path = lambda content: str(path)
        received = adapter.convert_message(BACKEND, self.access, GROUP, self.access.db.rows[-1])
        self.assertEqual(received['media'][0]['kind'], 'gif')

    def test_rejected_is_failed_lost_ack_is_uncertain_and_neither_resubmits(self):
        for mode, status in [('rejected', 'failed'), ('lost', 'uncertain')]:
            self.access.native.mode = mode
            message = self.message()
            ack = self.job(message).advance()
            self.assertEqual(ack['status'], status)
            count = len(self.access.native.calls)
            self.assertEqual(self.job(message).advance(), ack)
            self.assertEqual(len(self.access.native.calls), count)

    def test_partial_send_cannot_be_reported_as_wholly_failed(self):
        job = self.job(self.message(media=[self.attachment()]))
        self.assertIsNone(job.advance())
        self.access.native.mode = 'lost'
        ack = job.advance()
        self.assertEqual(ack['status'], 'uncertain')
        self.assertEqual(len(ack['source_ids']), 1)
        self.assertTrue(job.folder.exists())

    def test_confirmation_pending_can_continue_without_native_resubmission(self):
        self.access.native.mode = 'pending'
        message = self.message()
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
            self.assertEqual(len(self.access.native.calls), 1)
            self.access.native.echo(message['text'])
            ack = job.advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertEqual(len(self.access.native.calls), 1)

    def test_unconfirmed_echo_is_deferred_without_hiding_other_manual_messages(self):
        self.access.native.mode = 'pending'
        message = self.message(text='@Nick\u2005你好', mentions=[{'id': 'member-a', 'name': 'Nick'}])
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
        echo = row(sequence=201, source='<msgsource><atuserlist>member-a</atuserlist></msgsource>',
            content=message['text'])
        self.assertTrue(job.defers(GROUP, echo))
        self.assertFalse(job.defers('different@chatroom', echo))
        self.assertFalse(job.defers(GROUP, dict(echo, content='另一条手动发言')))
        self.assertFalse(job.defers(GROUP, dict(echo, sender_id=9, sender_username='member-a')))
        self.assertFalse(job.defers(GROUP, dict(echo, source='<msgsource/>')))
        self.access.db.rows.append(echo)
        ack = job.advance()
        self.assertEqual(ack['status'], 'sent')
        self.assertTrue(self.receipts.is_echo(BACKEND.wx_key(GROUP, echo)))
        self.assertFalse(job.defers(GROUP, echo))
        self.assertEqual(len(self.access.native.calls), 1)

    def test_mention_echo_without_actual_at_ids_cannot_confirm(self):
        self.access.native.mode = 'pending'
        message = self.message(text='@Nick\u2005你好', mentions=[{'id': 'member-b', 'name': 'Nick'}])
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
            self.access.db.rows.append(row(sequence=201, content=message['text'], source='<msgsource/>'))
            self.assertIsNone(job.advance())
        self.assertFalse(self.receipts.is_echo(BACKEND.wx_key(GROUP, self.access.db.rows[-1])))

    def test_sender_id_alone_cannot_confirm_another_members_message(self):
        self.access.native.mode = 'pending'
        message = self.message()
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
            self.access.db.rows.append(row(sequence=200, content='member-a:\n你好', sender_username='member-a'))
            self.assertIsNone(job.advance())
        self.assertFalse(self.receipts.is_echo(BACKEND.wx_key(GROUP, self.access.db.rows[-1])))
        self.assertEqual(len(self.access.native.calls), 1)

    def test_receive_journal_survives_restart_until_relay_receipt(self):
        self.access.db.rows = [row(sequence=10, local_id=1), row(sequence=11, local_id=2)]
        self.receipts.add_echo(BACKEND.wx_key(GROUP, self.access.db.rows[0]))
        position, more = adapter.journal_page(BACKEND, self.access, self.receipts, GROUP,
            {'sort_seq': 0, 'shard': '', 'local_id': -1})
        self.assertFalse(more)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 11)
        other = adapter.Receipts(self.directory / 'receipts.sqlite')
        try:
            pending = other.db.execute('SELECT source FROM pending').fetchall()
            self.assertEqual(pending, [(BACKEND.wx_key(GROUP, self.access.db.rows[1]),)])
            other.acknowledge('different@chatroom', pending[0][0])
            self.assertEqual(other.db.execute('SELECT count(*) FROM pending').fetchone()[0], 1)
            self.assertFalse(other.is_delivered(pending[0][0]))
            other.acknowledge(GROUP, pending[0][0])
            self.assertEqual(other.db.execute('SELECT count(*) FROM pending').fetchone()[0], 0)
            self.assertTrue(other.is_delivered(pending[0][0]))
        finally:
            other.db.close()

    def test_journal_transaction_does_not_advance_cursor_after_bad_row(self):
        self.access.db.rows = [row(sequence=10), row(sequence=11, local_id=2)]
        original = BACKEND.wx_key
        def broken(group, message):
            if message['local_id'] == 2:
                raise RuntimeError('source not ready')
            return original(group, message)
        backend = types.SimpleNamespace(wx_key=broken, wechat_system_message=BACKEND.wechat_system_message)
        with self.assertRaises(RuntimeError):
            adapter.journal_page(backend, self.access, self.receipts, GROUP,
                {'sort_seq': 0, 'shard': '', 'local_id': -1})
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 0)
        self.assertIsNone(self.receipts.state('cursor:' + GROUP))

    def test_unopened_groups_receive_live_messages_without_native_authorization(self):
        other_group = '987654@chatroom'
        self.access.db.groups_rows = {GROUP: [row(sequence=10)], other_group: [row(sequence=20)]}
        watched = adapter.initialize_watches(BACKEND, self.access, self.receipts,
            [{'id': GROUP}, {'id': other_group}])
        self.assertEqual(watched, {GROUP, other_group})
        self.assertFalse(self.access.native.calls)
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 0)
        self.access.db.groups_rows[other_group].append(row(sequence=21, local_id=2))
        for group in watched:
            adapter.journal_page(BACKEND, self.access, self.receipts, group, adapter.watch_position(self.receipts, group))
        pending = self.receipts.db.execute('SELECT group_id,source FROM pending').fetchall()
        self.assertEqual(pending, [(other_group, BACKEND.wx_key(other_group, self.access.db.groups_rows[other_group][-1]))])
        self.assertFalse(self.access.native.calls)

    def test_opening_backfills_history_once_without_rewinding_or_repeating_receipts(self):
        self.access.db.rows = [row(sequence=index, local_id=index) for index in range(1, 61)]
        adapter.initialize_watches(BACKEND, self.access, self.receipts, [{'id': GROUP}])
        self.access.db.rows.append(row(sequence=61, local_id=61))
        adapter.journal_page(BACKEND, self.access, self.receipts, GROUP, adapter.watch_position(self.receipts, GROUP))
        new_source = BACKEND.wx_key(GROUP, self.access.db.rows[-1])
        self.receipts.acknowledge(GROUP, new_source)
        self.assertEqual(adapter.backfill_group(BACKEND, self.access, self.receipts, GROUP), 39)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 61)
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 39)
        self.assertEqual(adapter.backfill_group(BACKEND, self.access, self.receipts, GROUP), 0)
        for (source,) in self.receipts.db.execute('SELECT source FROM pending').fetchall():
            self.receipts.acknowledge(GROUP, source)
        adapter.journal_page(BACKEND, self.access, self.receipts, GROUP, adapter.watch_position(self.receipts, GROUP))
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 0)

    def test_watch_restart_preserves_cursor_and_unacknowledged_backlog(self):
        self.access.db.rows = [row(sequence=10)]
        adapter.initialize_watches(BACKEND, self.access, self.receipts, [{'id': GROUP}])
        self.access.db.rows.append(row(sequence=11, local_id=2))
        adapter.journal_page(BACKEND, self.access, self.receipts, GROUP, adapter.watch_position(self.receipts, GROUP))
        self.access.db.rows.append(row(sequence=12, local_id=3))
        restarted = adapter.Receipts(self.directory / 'receipts.sqlite')
        try:
            with patch.object(self.access.db, 'get_messages', side_effect=AssertionError('old cursor must not rebaseline')):
                adapter.initialize_watches(BACKEND, self.access, restarted, [{'id': GROUP}])
            self.assertEqual(restarted.state('cursor:' + GROUP), 11)
            adapter.journal_page(BACKEND, self.access, restarted, GROUP, adapter.watch_position(restarted, GROUP))
            self.assertEqual(restarted.state('cursor:' + GROUP), 12)
            self.assertEqual(restarted.db.execute('SELECT count(*) FROM pending').fetchone()[0], 2)
            self.assertFalse(restarted.state('history_loaded:' + GROUP, False))
        finally:
            restarted.db.close()

    def test_refresh_adds_new_groups_without_resetting_existing_cursor(self):
        other_group = '987654@chatroom'
        self.access.db.groups_rows = {GROUP: [row(sequence=10)], other_group: [row(sequence=88)]}
        adapter.initialize_watches(BACKEND, self.access, self.receipts, [{'id': GROUP}])
        self.receipts.set_state('cursor:' + GROUP, 50)
        watched = adapter.initialize_watches(BACKEND, self.access, self.receipts,
            [{'id': GROUP}, {'id': other_group}])
        self.assertEqual(watched, {GROUP, other_group})
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 50)
        self.assertEqual(self.receipts.state('cursor:' + other_group), 88)
        self.access.db.groups_rows[other_group].append(row(sequence=89, local_id=2))
        adapter.journal_page(BACKEND, self.access, self.receipts, other_group, adapter.watch_position(self.receipts, other_group))
        self.assertEqual(self.receipts.db.execute('SELECT group_id FROM pending').fetchall(), [(other_group,)])

    def test_equal_sequence_baseline_ignores_old_rows_but_retains_new_boundary_rows(self):
        self.access.db.rows = [row(sequence=99, local_id=index) for index in range(1, 503)]
        adapter.initialize_watches(BACKEND, self.access, self.receipts, [{'id': GROUP}])
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM baseline').fetchone()[0], 502)
        self.access.db.rows.extend([row(sequence=99, local_id=503), row(sequence=100, local_id=504)])
        position = adapter.watch_position(self.receipts, GROUP)
        while True:
            position, more = adapter.journal_page(BACKEND, self.access, self.receipts, GROUP, position)
            if not more:
                break
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 2)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 100)

    def test_empty_group_can_be_watched_before_its_first_message(self):
        adapter.initialize_watches(BACKEND, self.access, self.receipts, [{'id': GROUP}])
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 0)
        self.access.db.rows.append(row(sequence=1))
        adapter.journal_page(BACKEND, self.access, self.receipts, GROUP, adapter.watch_position(self.receipts, GROUP))
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 1)

    def test_explicit_send_can_restore_authorization_after_restart_but_unknown_group_cannot(self):
        active, calls = set(), []
        connect = lambda groups: calls.append(set(groups))
        adapter.authorize_sender(GROUP, {GROUP}, active, connect)
        self.assertEqual(active, {GROUP})
        self.assertEqual(calls, [{GROUP}])
        job = adapter.OutgoingMessage(BACKEND, self.access, self.receipts, self.data,
            self.directory, active, self.message())
        self.assertEqual(job.advance()['status'], 'sent')
        adapter.authorize_sender(GROUP, {GROUP}, active, connect)
        self.assertEqual(len(calls), 1)
        with self.assertRaises(ValueError):
            adapter.authorize_sender('unknown@chatroom', {GROUP}, active, connect)
        self.assertEqual(len(calls), 1)
        self.assertEqual(active, {GROUP})

    def test_default_empty_forwarding_publishes_catalog_without_reading_messages(self):
        result = self.run_main_fixture()
        self.assertEqual(len(next(event['groups'] for event in result.events if event['type'] == 'groups')), 2)
        self.assertFalse(result.reads)
        self.assertFalse(result.connections)
        self.assertFalse(any(event['type'] == 'message' for event in result.events))
        self.assertEqual(self.receipts.db.execute("SELECT count(*) FROM state WHERE key LIKE 'cursor:%'").fetchone()[0], 0)

    def test_restart_replays_selected_offline_backlog_without_another_file_change(self):
        self.receipts.set_state('cursor:' + GROUP, 5)
        result = self.run_main_fixture([{'op': 'configure', 'groups': [GROUP]}], selected=json.dumps([GROUP]))
        messages = [event['message'] for event in result.events if event['type'] == 'message']
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0]['group'], GROUP)
        self.assertEqual(messages[0]['source_id'], BACKEND.wx_key(GROUP, row(sequence=10)))
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 10)
        self.assertFalse(result.connections)

    def test_only_selected_group_is_baselined_and_unselected_activation_is_denied(self):
        result = self.run_main_fixture([{'op': 'activate', 'group': '987654@chatroom'}], selected=json.dumps([GROUP]))
        self.assertEqual(result.reads, [GROUP])
        self.assertFalse(result.connections)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 10)
        self.assertIsNone(self.receipts.state('cursor:987654@chatroom'))
        self.assertTrue(any(event['type'] == 'warning' and event.get('group') == '987654@chatroom' for event in result.events))

    def test_configure_selection_is_hot_and_does_not_authorize_native(self):
        other_group = '987654@chatroom'
        result = self.run_main_fixture([{'op': 'configure', 'groups': [other_group]}, {'op': 'configure', 'groups': []}],
            selected=json.dumps([GROUP]))
        self.assertEqual(result.reads, [GROUP, other_group])
        self.assertFalse(result.connections)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 10)
        self.assertEqual(self.receipts.state('cursor:' + other_group), 20)
        statuses = [event['text'] for event in result.events if event['type'] == 'status']
        self.assertIn('0 个群', statuses[-1])

    def test_hot_cancel_shrinks_loaded_native_allowlist_and_denies_unselected_send(self):
        other_group = '987654@chatroom'
        message = self.message(group=GROUP)
        result = self.run_main_fixture([{'op': 'activate', 'group': GROUP},
            {'op': 'activate', 'group': other_group}, {'op': 'configure', 'groups': [other_group]},
            {'op': 'send', 'message': message}, {'op': 'configure', 'groups': []}],
            selected=json.dumps([GROUP, other_group]))
        self.assertEqual(result.connections, [{GROUP}, {GROUP, other_group}])
        self.assertEqual(result.allowlists_at_status[-2:], [[[other_group]], [[]]])
        self.assertTrue(any(event['type'] == 'ack' and event['id'] == message['id'] and event['status'] == 'failed'
            for event in result.events))
        self.assertTrue(all(not native.calls for native in result.instances))

    def test_hot_cancel_stops_unstarted_jobs_while_preserving_submitted_attempt(self):
        first = self.message(media=[self.attachment()])
        second = self.message(text='第二条未提交')
        result = self.run_main_fixture([{'op': 'send', 'message': first}, {'op': 'send', 'message': second},
            {'op': 'configure', 'groups': []}], selected=json.dumps([GROUP]), native_mode='pending')
        self.assertEqual(sum(len(native.calls) for native in result.instances), 1)
        self.assertTrue(self.receipts.state('outgoing:' + first['id'])['cancelled'])
        self.assertEqual(self.receipts.state('outgoing:' + second['id'])['ack']['status'], 'failed')
        self.assertEqual(result.allowlists_at_status[-1], [[]])

    def test_catalog_refresh_does_not_automatically_select_a_new_local_group(self):
        new_group = '555555@chatroom'
        catalog = [{'id': GROUP, 'name': '本机甲群'}, {'id': '987654@chatroom', 'name': '本机乙群'},
            {'id': new_group, 'name': '新加入群'}]
        with patch.object(adapter.time, 'monotonic', side_effect=[100, 200, 200]):
            result = self.run_main_fixture([{'op': 'noop'}], selected=json.dumps([GROUP]), refreshed_catalog=catalog)
        self.assertEqual(result.reads, [GROUP])
        self.assertIsNone(self.receipts.state('cursor:' + new_group))
        self.assertEqual(len([event for event in result.events if event['type'] == 'groups'][-1]['groups']), 3)
        self.assertFalse(result.connections)

    def test_selection_unknown_ids_do_not_gain_access_and_malformed_policy_is_rejected(self):
        catalog = [{'id': GROUP}]
        self.assertEqual(adapter.forwarding_selection([GROUP, GROUP, 'unknown@chatroom'], catalog), {GROUP})
        self.assertEqual(adapter.forwarding_selection('[]', catalog), set())
        for invalid in ('not-json', '{}', [1], [None], [''], ['bad\x00id']):
            with self.assertRaises(ValueError):
                adapter.forwarding_selection(invalid, catalog)
        result = self.run_main_fixture([{'op': 'configure', 'groups': '[]'},
            {'op': 'activate', 'group': 'unknown@chatroom'}], selected='["unknown@chatroom"]')
        self.assertFalse(result.reads)
        self.assertFalse(result.connections)

    def test_empty_policy_preserves_old_cursor_pending_and_clears_old_native_allowlists(self):
        self.receipts.set_state('cursor:' + GROUP, 50)
        message = row(sequence=51)
        with self.receipts.db:
            adapter.journal_row(BACKEND, self.receipts, GROUP, message)
        config = self.directory / 'native' / 'old-hash' / 'native.json'
        config.parent.mkdir(parents=True)
        original = {'groups': [GROUP], 'token': 'secret-fixture', 'port': 30001, 'media_root': 'fixture'}
        config.write_text(json.dumps(original), 'utf-8')
        result = self.run_main_fixture()
        self.assertFalse(result.reads)
        self.assertFalse(result.connections)
        self.assertEqual(self.receipts.state('cursor:' + GROUP), 50)
        self.assertEqual(self.receipts.db.execute('SELECT count(*) FROM pending').fetchone()[0], 1)
        current = json.loads(config.read_text('utf-8'))
        self.assertEqual(current, dict(original, groups=[]))

    def test_native_allowlists_shrink_to_empty_and_never_modify_other_service(self):
        configs = [self.directory / 'native' / 'native.json', self.directory / 'native' / 'hash' / 'native.json']
        sibling = self.data / 'other-service' / 'native' / 'native.json'
        for config in [*configs, sibling]:
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text(json.dumps({'groups': [GROUP, '987654@chatroom'], 'token': 'fixture', 'port': 1234}), 'utf-8')
        adapter.restrict_native_groups(self.directory, {GROUP})
        self.assertTrue(all(json.loads(config.read_text('utf-8'))['groups'] == [GROUP] for config in configs))
        adapter.restrict_native_groups(self.directory, set())
        self.assertTrue(all(json.loads(config.read_text('utf-8'))['groups'] == [] for config in configs))
        self.assertEqual(json.loads(sibling.read_text('utf-8'))['groups'], [GROUP, '987654@chatroom'])

    def test_service_exit_revokes_even_a_newly_authorized_native_module(self):
        result = self.run_main_fixture([{'op': 'activate', 'group': GROUP}], selected=json.dumps([GROUP]))
        self.assertEqual(result.connections, [{GROUP}])
        self.assertTrue(result.instances)
        config = self.directory / 'native' / 'fixture-hash' / 'native.json'
        self.assertEqual(json.loads(config.read_text('utf-8'))['groups'], [])
        self.assertFalse(result.instances[0].calls)

    def test_cancel_unstarted_job_fails_and_duplicate_request_does_not_submit(self):
        message = self.message(media=[self.attachment()])
        job = self.job(message)
        kept, results = adapter.cancel_unselected([job], set())
        self.assertEqual(kept, [])
        self.assertEqual(results[0]['status'], 'failed')
        self.assertFalse(self.access.native.calls)
        self.assertEqual(self.job(message).advance(), results[0])
        self.assertFalse(self.access.native.calls)

    def test_cancel_after_first_part_does_not_send_remaining_attachment(self):
        job = self.job(self.message(media=[self.attachment()]))
        self.assertIsNone(job.advance())
        kept, results = adapter.cancel_unselected([job], set())
        self.assertFalse(kept)
        self.assertEqual(results[0]['status'], 'uncertain')
        self.assertEqual(len(results[0]['source_ids']), 1)
        self.assertEqual(len(self.access.native.calls), 1)

    def test_cancel_pending_part_only_confirms_and_never_submits_next_part(self):
        self.access.native.mode = 'pending'
        message = self.message(media=[self.attachment()])
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
        kept, results = adapter.cancel_unselected([job], set())
        self.assertEqual(kept, [job])
        self.assertFalse(results)
        self.access.native.echo(message['text'])
        result = job.advance()
        self.assertEqual(result['status'], 'uncertain')
        self.assertEqual(len(result['source_ids']), 1)
        self.assertEqual(len(self.access.native.calls), 1)

    def test_cancel_pending_last_part_can_confirm_without_resubmission(self):
        self.access.native.mode = 'pending'
        message = self.message()
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
        self.assertIsNone(job.cancel())
        self.access.native.echo(message['text'])
        self.assertEqual(job.advance()['status'], 'sent')
        self.assertEqual(len(self.access.native.calls), 1)

    def test_cancelled_pending_attempt_survives_restart_and_times_out_without_resubmission(self):
        self.access.native.mode = 'pending'
        message = self.message(media=[self.attachment()])
        job = self.job(message)
        with patch('bridge.time.sleep', return_value=None):
            self.assertIsNone(job.advance())
        self.assertIsNone(job.cancel())
        state_key = 'wx_attempt:' + message['id'] + ':0'
        state = self.receipts.state(state_key)
        state['await_started'] = time.time() - 31
        self.receipts.set_state(state_key, state)
        restored = self.job(message)
        self.assertTrue(restored.record['cancelled'])
        self.assertEqual(restored.advance()['status'], 'uncertain')
        self.assertEqual(len(self.access.native.calls), 1)

    def test_voice_silk_to_wav_roundtrip_preserves_original(self):
        import pilk
        pcm = self.data / 'fixture.pcm'
        samples = [int(12000 * math.sin(2 * math.pi * 440 * sample / 24000)) for sample in range(12000)]
        pcm.write_bytes(struct.pack('<' + 'h' * len(samples), *samples))
        silk = self.data / 'voice.silk'
        pilk.encode(str(pcm), str(silk), pcm_rate=24000, tencent=True)
        original = silk.read_bytes()
        self.assertTrue(original.startswith(b'\x02#!SILK_V3'))
        wav_path = adapter.decode_voice(silk)
        with wave.open(str(wav_path), 'rb') as decoded:
            self.assertEqual(decoded.getparams()[:3], (1, 2, 24000))
            self.assertGreater(decoded.getnframes(), 10000)
            self.assertLess(decoded.getnframes(), 14000)
            self.assertTrue(any(decoded.readframes(10000)))
        self.assertEqual(silk.read_bytes(), original)
        self.assertFalse(silk.with_suffix('.pcm.part').exists())
        modified = wav_path.stat().st_mtime_ns
        self.assertEqual(adapter.decode_voice(silk), wav_path)
        self.assertEqual(wav_path.stat().st_mtime_ns, modified)
        self.access.media_path = lambda group, message, kind: str(silk) if kind == 'record' else None
        received = adapter.convert_message(BACKEND, self.access, GROUP, row(type='语音', content='[语音]'))
        self.assertEqual(received['media'][0]['kind'], 'voice')
        self.assertEqual(received['media'][0]['path'], str(wav_path.resolve()))


class CursorTests(unittest.TestCase):
    def test_polling_groups_reuses_readonly_message_connections_and_releases_them(self):
        db = MemoryWeChatDB.__new__(MemoryWeChatDB)
        db._scope = threading.local()
        db._snapshots, db._message_senders, db._group_names = {}, {}, {}
        connections = [sqlite3.connect(':memory:', factory=MemoryConnection) for _ in range(2)]
        for connection in connections:
            connection.owner = threading.get_ident()
            self.addCleanup(connection.close)
        db._connections = weakref.WeakSet(connections)
        db._message_dbs = lambda: ['a.db', 'b.db']
        db._find_msg_tables = lambda group, conns: [(conn, 'messages') for conn in conns]
        with patch.object(db, '_open', side_effect=connections) as opened:
            with db.snapshot_scope():
                for group in [GROUP, 'other@chatroom']:
                    self.assertEqual(db._run_msg_query(group, lambda found: len(found)), 2)
                self.assertEqual(opened.call_count, 2)
                self.assertEqual(connections[0].execute('SELECT 1').fetchone()[0], 1)
        self.assertIsNone(db._scope.message_connections)
        for connection in connections:
            with self.assertRaises(sqlite3.ProgrammingError):
                connection.execute('SELECT 1')

    def test_absent_group_message_table_returns_an_empty_page(self):
        db = MemoryWeChatDB.__new__(MemoryWeChatDB)
        db._run_msg_query = lambda group, callback: None
        self.assertEqual(db.get_new_message_page(GROUP, {'sort_seq': 0, 'shard': '', 'local_id': -1}), [])

    def test_bounded_cursor_reads_all_equal_sequences_across_shards(self):
        db = MemoryWeChatDB.__new__(MemoryWeChatDB)
        connections = []
        for shard in ('b.db', 'a.db'):
            connection = sqlite3.connect(':memory:')
            self.addCleanup(connection.close)
            connection.row_factory = sqlite3.Row
            connection.execute('CREATE TABLE messages(local_id INTEGER,local_type INTEGER,real_sender_id INTEGER,'
                'create_time INTEGER,message_content TEXT,source TEXT,packed_info_data BLOB,compress_content BLOB,'
                'server_id INTEGER,sort_seq INTEGER)')
            connection.executemany('INSERT INTO messages VALUES(?,1,2,1760000000,\'text\',\'\',NULL,NULL,1,99)',
                [(index,) for index in range(1, 252)])
            connections.append((connection, 'messages', shard))
        db._shard_label = lambda conn: next(shard for item, _, shard in connections if item is conn)
        db._run_msg_query = lambda group, callback: callback([(conn, table) for conn, table, _ in connections])
        db._msg_row_to_dict = lambda item: dict(item)
        position = {'sort_seq': 98, 'shard': '', 'local_id': -1}
        seen = []
        while True:
            page = db.get_new_message_page(GROUP, position, limit=200)
            self.assertLessEqual(len(page), 200)
            if not page:
                break
            seen.extend((item['shard'], item['local_id']) for item in page)
            position = {key: page[-1][key] for key in ('sort_seq', 'shard', 'local_id')}
        self.assertEqual(len(seen), 502)
        self.assertEqual(len(set(seen)), 502)
        self.assertEqual(seen, sorted(seen))


if __name__ == '__main__':
    unittest.main()

"""Local Windows WeChat adapter. NDJSON on stdin/stdout; no sends on import."""
from __future__ import annotations

import base64
import contextlib
from functools import lru_cache
import hashlib
import importlib.util
import io
import json
import logging
import os
from pathlib import Path
import queue
import re
import shutil
import sqlite3
import sys
import threading
import time
import types
import uuid
import wave

ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / 'vendor'
sys.path.insert(0, str(VENDOR))


def configure_stdio():
    # Windows pipes may otherwise use the user's ANSI code page instead of the relay's UTF-8 protocol.
    for stream in (sys.stdin, sys.stdout, sys.stderr, sys.__stdout__):
        if stream is not None and hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='strict')


def emit(value):
    sys.__stdout__.write(json.dumps(value, ensure_ascii=False) + '\n')
    sys.__stdout__.flush()


def load_backend():
    # Only the database/media modules are needed; the package entry point also loads OpenCV and GUI automation.
    package = types.ModuleType('wechatauto')
    spec = importlib.util.find_spec('wechatauto')
    if spec is None or not spec.submodule_search_locations:
        raise RuntimeError('缺少 wechatauto-replica 依赖')
    package.__path__ = list(spec.submodule_search_locations)
    package.__spec__ = spec
    sys.modules['wechatauto'] = package
    from wechatauto.param import WxParam
    WxParam.ENABLE_FILE_LOGGER = False
    logger = logging.getLogger('wechat-lite.disabled')
    logger.disabled = True
    logger.logger = logger
    logger_module = types.ModuleType('wechatauto.logger')
    logger_module.wxlog = logger
    sys.modules['wechatauto.logger'] = logger_module
    from wechatauto.media import MediaDownloader
    package.WxParam, package.wxlog, package.MediaDownloader = WxParam, logger, MediaDownloader
    from bridge import WeChatAccess, Unavailable, MediaPending, Uncertain, ConfirmationPending, wx_key, xml_root, text_at, wechat_system_message
    from native_sender import NativeClient, main_process
    return types.SimpleNamespace(**locals())


class Receipts:
    def __init__(self, path):
        self.db = sqlite3.connect(path)
        self.db.executescript('PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY,value TEXT);'
            'CREATE TABLE IF NOT EXISTS echoes(key TEXT PRIMARY KEY);'
            'CREATE TABLE IF NOT EXISTS pending(source TEXT PRIMARY KEY,group_id TEXT,locator TEXT,due REAL DEFAULT 0);'
            'CREATE TABLE IF NOT EXISTS delivered(source TEXT PRIMARY KEY,group_id TEXT NOT NULL);'
            'CREATE TABLE IF NOT EXISTS baseline(source TEXT PRIMARY KEY,group_id TEXT NOT NULL);'
            'CREATE INDEX IF NOT EXISTS pending_due ON pending(due);')
        self.warned = set()

    def state(self, key, default=None):
        row = self.db.execute('SELECT value FROM state WHERE key=?', (key,)).fetchone()
        return json.loads(row[0]) if row else default

    def set_state(self, key, value):
        with self.db:
            self.db.execute('INSERT OR REPLACE INTO state VALUES(?,?)', (key, json.dumps(value, ensure_ascii=False)))

    def add_echo(self, key):
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO echoes VALUES(?)', (key,))

    def is_echo(self, key):
        return bool(self.db.execute('SELECT 1 FROM echoes WHERE key=?', (key,)).fetchone())

    def is_delivered(self, key):
        return bool(self.db.execute('SELECT 1 FROM delivered WHERE source=?', (key,)).fetchone())

    def is_baseline(self, key):
        return bool(self.db.execute('SELECT 1 FROM baseline WHERE source=?', (key,)).fetchone())

    def submitted(self, ident):
        prefix = 'wx_attempt:' + ident + ':'
        rows = self.db.execute('SELECT value FROM state WHERE substr(key,1,?)=?', (len(prefix), prefix))
        return any(not json.loads(row[0]).get('rejected') for row in rows)

    def acknowledge(self, group, source):
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO delivered SELECT source,group_id FROM pending WHERE source=? AND group_id=?', (source, group))
            self.db.execute('DELETE FROM pending WHERE source=? AND group_id=?', (source, group))
        self.warned.discard(source)


def decode_voice(path):
    """Decode an adapter-owned SILK file, keeping the original and committing WAV atomically."""
    path = Path(path)
    wav = path.with_suffix('.wav')
    if wav.is_file():
        try:
            with wave.open(str(wav), 'rb') as cached:
                if cached.getparams()[:3] == (1, 2, 24000) and cached.getnframes() > 0:
                    return wav
        except (EOFError, wave.Error):
            pass
    import pilk
    pcm = path.with_suffix('.pcm.part')
    temporary = wav.with_suffix('.wav.part')
    try:
        pilk.decode(str(path), str(pcm), pcm_rate=24000)
        if not pcm.is_file() or pcm.stat().st_size == 0 or pcm.stat().st_size % 2:
            raise ValueError('微信语音尚未完整解码')
        with wave.open(str(temporary), 'wb') as output:
            output.setparams((1, 2, 24000, 0, 'NONE', 'NONE'))
            with pcm.open('rb') as source_file:
                while chunk := source_file.read(65536):
                    output.writeframesraw(chunk)
        temporary.replace(wav)
        return wav
    finally:
        pcm.unlink(missing_ok=True)
        temporary.unlink(missing_ok=True)


def message_mentions(backend, access, group, message):
    source = message.get('source')
    if not source:
        return []
    root = backend.xml_root(source)
    value = backend.text_at(root, 'atuserlist') if root is not None else ''
    result = []
    for ident in dict.fromkeys(value.split(',')):
        ident = ident.strip()
        if ident:
            result.append({'id': ident, 'name': '所有人' if ident == 'notify@all' else access.db.get_display_name(group, ident) or '群成员'})
    return result


def timestamp_ms(value):
    if value is None:
        return int(time.time() * 1000)
    try:
        stamp = float(value)
    except (TypeError, ValueError):
        from datetime import datetime
        stamp = datetime.fromisoformat(str(value).replace('Z', '+00:00')).timestamp()
    if abs(stamp) < 10_000_000_000:
        stamp *= 1000
    return int(stamp)


@lru_cache(maxsize=512)
def avatar_data(db, username):
    """Optional local avatar, bounded both in pixels and in cache entries; never fetch a URL."""
    if not username:
        return ''
    try:
        from PIL import Image
        content = db.get_avatar(username)
        if not isinstance(content, bytes) or not content or len(content) > 2 * 1024 * 1024:
            return ''
        with Image.open(io.BytesIO(content)) as image:
            if not 0 < image.width <= 2048 or not 0 < image.height <= 2048:
                return ''
            image.thumbnail((64, 64), Image.Resampling.LANCZOS)
            if image.mode in ('RGBA', 'LA') or 'transparency' in image.info:
                rgba = image.convert('RGBA')
                thumbnail = Image.new('RGB', rgba.size, 'white')
                thumbnail.paste(rgba, mask=rgba.getchannel('A'))
            else:
                thumbnail = image.convert('RGB')
            output = io.BytesIO()
            thumbnail.save(output, format='JPEG', quality=82)
        return base64.b64encode(output.getvalue()).decode('ascii')
    except Exception:
        # A missing or partially written avatar must not delay message delivery.
        return ''


def group_list(access):
    return [dict(group, avatar=avatar_data(access.db, group['id'])) for group in access.groups()]


def member_list(access, group):
    result = []
    for member in access.db.get_group_members(group):
        ident = member['username']
        name = access.db.get_display_name(group, ident) or member.get('remark') or member.get('nick_name') or '群成员'
        result.append({'id': ident, 'name': name, 'remark': member.get('remark') or '', 'avatar': avatar_data(access.db, ident)})
    return result


def convert_message(backend, access, group, message):
    if backend.wechat_system_message(message):
        return None
    content, username = access.message_identity(group, message)
    name = access.db.get_display_name(group, username) if username else '群成员'
    name = name if name and name != username else ('我' if message.get('sender_id') == 2 else '群成员')
    source = backend.wx_key(group, message)
    media = []
    text = content if message.get('type') == '文本' else ''
    kind = message.get('type')
    if kind == '动画表情':
        path, media_kind = access.emoji_path(content), 'gif'
    elif kind in ('图片', '语音', '视频'):
        media_kind = {'图片': 'image', '语音': 'voice', '视频': 'video'}[kind]
        path = access.media_path(group, message, {'voice': 'record'}.get(media_kind, media_kind))
    elif kind != '文本':
        root = backend.xml_root(content)
        app = root.find('.//appmsg') if root is not None else None
        if app is not None and backend.text_at(app, 'type') in ('6', '74'):
            path, media_kind = access.media_path(group, message, 'file'), 'file'
        else:
            # Preserve readable context for cards without exposing internal XML or marking them as sent files.
            pieces = [backend.text_at(root, key) for key in ('title', 'des', 'url')] if root is not None else []
            text = '\n'.join(dict.fromkeys(p for p in pieces if p)) or f'[{kind or "暂不支持的消息"}]'
            path, media_kind = None, None
    else:
        path, media_kind = None, None
    if path:
        path = Path(path)
        if media_kind == 'voice' and path.suffix.lower() == '.silk':
            path = decode_voice(path)
        if media_kind == 'gif':
            with path.open('rb') as file:
                if file.read(6) not in (b'GIF87a', b'GIF89a'):
                    media_kind = 'image'
        media.append({'id': '', 'name': path.name, 'kind': media_kind, 'size': path.stat().st_size, 'sha256': '', 'path': str(path.resolve())})
    timestamp = timestamp_ms(message.get('create_time', message.get('timestamp')))
    return {'id': str(uuid.uuid5(uuid.NAMESPACE_URL, source)), 'group': group, 'sender': name, 'sender_id': username or '', 'avatar': avatar_data(access.db, username),
        'text': text, 'media': media, 'mentions': message_mentions(backend, access, group, message), 'timestamp': timestamp, 'status': 'received', 'source_id': source,
        'outgoing': username == access.db.wxid if username else message.get('sender_id') == 2}


class OutgoingMessage:
    """One explicitly requested message; confirmation slices never resubmit a native request."""
    def __init__(self, backend, access, receipts, data, directory, active, message):
        self.backend, self.access, self.receipts = backend, access, receipts
        self.message, self.index, self.steps = message, 0, []
        ident = message['id']
        folder_name = str(uuid.UUID(ident))
        self.key = 'outgoing:' + ident
        self.folder = directory / 'feishu' / 'media' / folder_name
        expected = {key: message.get(key) for key in ('group', 'text', 'mentions', 'media')}
        fingerprint = hashlib.sha256(json.dumps(expected, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        self.record = receipts.state(self.key)
        if self.record and self.record['hash'] != fingerprint:
            raise ValueError('重复发送编号的内容不一致，未重新提交')
        if self.record and self.record.get('ack'):
            return
        if message['group'] not in active:
            raise ValueError('请先打开目标群再发送')
        roster = {m['id']: m for m in member_list(access, message['group'])}
        if len(message.get('media', [])) > 12 or len(message.get('mentions', [])) > 100:
            raise ValueError('附件或 @ 人数超过容量限制')
        names = {}
        for mention in message.get('mentions', []):
            if mention['id'] not in roster or roster[mention['id']]['name'] != mention['name']:
                raise ValueError('@ 成员已变化，请重新选择')
            token = '@' + mention['name']
            if token not in message.get('text', ''):
                raise ValueError('@ 成员与正文不一致，请重新选择')
            names[mention['id']] = token + '\u2005'
        text = message.get('text', '')
        if '\0' in text or len(text.encode('utf-8')) > 24000:
            raise ValueError('微信文本超过发送容量或包含无效字符')
        if text.strip():
            self.steps.append({'kind': 'text', 'text': text, 'atlist': ','.join(names), 'atnames': names})
        media_root = (data / 'media').resolve()
        for index, media in enumerate(message.get('media', [])):
            original = Path(media['path']).resolve()
            name = media['name']
            if not name or Path(name).name != name or name in ('.', '..') or re.search(r'[<>:"/\\|?*\x00-\x1f]', name):
                raise ValueError('附件名称无效')
            if not original.is_relative_to(media_root) or not original.is_file():
                raise ValueError('附件不在当前服务端的媒体目录')
            if not 0 < original.stat().st_size <= 512 * 1024 * 1024:
                raise ValueError('附件为空或超过 512 MiB')
            destination = self.folder / str(index) / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.is_file():
                shutil.copyfile(original, destination)
            with destination.open('rb') as file:
                if hashlib.file_digest(file, 'sha256').hexdigest() != media['sha256'] or destination.stat().st_size != media['size']:
                    raise ValueError('附件内容校验失败')
            self.steps.append({'kind': 'image' if media['kind'] in ('image', 'gif') else 'file', 'path': str(destination)})
        if not self.steps:
            raise ValueError('消息为空')
        self.record = self.record or {'hash': fingerprint}
        self.index = int(self.record.get('index', 0))
        receipts.set_state(self.key, self.record)

    def finish(self, status, error=''):
        ident = self.message['id']
        sources = []
        for index in range(len(self.steps)):
            attempt = self.receipts.state('wx_attempt:' + ident + ':' + str(index), {})
            if attempt.get('ack'):
                sources.append(attempt['ack'])
        ack = {'type': 'ack', 'id': ident, 'status': status, 'source_id': sources[0] if sources else '', 'source_ids': sources}
        if error:
            ack['error'] = error
        self.record['ack'] = ack
        self.receipts.set_state(self.key, self.record)
        if status == 'sent' or not self.receipts.submitted(ident):
            shutil.rmtree(self.folder, ignore_errors=True)
        return ack

    def advance(self):
        if self.record.get('ack'):
            return self.record['ack']
        if self.record.get('cancelled'):
            state = self.receipts.state('wx_attempt:' + self.message['id'] + ':' + str(self.index))
            if not state or state.get('rejected'):
                return self.finish('uncertain' if self.receipts.submitted(self.message['id']) else 'failed',
                    '该群已取消转发，后续内容未提交微信')
        try:
            self.access.send(self.message['group'], self.steps[self.index], self.receipts,
                key=self.message['id'] + ':' + str(self.index))
            self.index += 1
            self.record['index'] = self.index
            self.receipts.set_state(self.key, self.record)
            if self.record.get('cancelled') and self.index < len(self.steps):
                return self.finish('uncertain', '该群已取消转发；已提交部分得到确认，后续内容未提交微信')
            return self.finish('sent') if self.index == len(self.steps) else None
        except self.backend.ConfirmationPending:
            return None
        except Exception as exc:
            return self.finish('uncertain' if self.receipts.submitted(self.message['id']) else 'failed', str(exc))

    def cancel(self):
        if self.record.get('ack'):
            return self.record['ack']
        self.record['cancelled'] = True
        self.receipts.set_state(self.key, self.record)
        state = self.receipts.state('wx_attempt:' + self.message['id'] + ':' + str(self.index))
        if state and not state.get('rejected'):
            return None  # advance() may only reconcile this existing attempt, never submit another part.
        return self.finish('uncertain' if self.receipts.submitted(self.message['id']) else 'failed',
            '该群已取消转发，后续内容未提交微信')

    def defers(self, group, message):
        if group != self.message['group']:
            return False
        state = self.receipts.state('wx_attempt:' + self.message['id'] + ':' + str(self.index), {})
        if not state.get('expected') or state.get('ack') or state.get('rejected') or message['sort_seq'] <= state['before']:
            return False
        return self.access.matches_outgoing(group, message, state['expected'])


def initialize_watches(backend, access, receipts, groups):
    """Start new groups at their live tail; persisted cursors keep their restart backlog."""
    watched = {group['id'] for group in groups}
    for group in sorted(watched):
        if receipts.state('cursor:' + group) is None:
            recent = access.db.get_messages(group, limit=1)
            floor = max((int(message['sort_seq']) for message in recent), default=0)
            with receipts.db:
                # Remember only the existing boundary rows, so a later row with equal sort_seq is still new.
                position = {'sort_seq': floor - 1, 'shard': '', 'local_id': -1}
                while recent:
                    rows = access.db.get_new_message_page(group, position, limit=200)
                    for row in rows:
                        if int(row['sort_seq']) == floor:
                            receipts.db.execute('INSERT OR IGNORE INTO baseline VALUES(?,?)', (backend.wx_key(group, row), group))
                    if len(rows) < 200 or int(rows[-1]['sort_seq']) > floor:
                        break
                    position = {key: rows[-1][key] for key in ('sort_seq', 'shard', 'local_id')}
                receipts.db.execute('INSERT INTO state VALUES(?,?)', ('cursor:' + group, json.dumps(floor)))
                receipts.db.execute('INSERT OR REPLACE INTO state VALUES(?,?)', ('watch_floor:' + group, json.dumps(floor)))
    return watched


def journal_row(backend, receipts, group, row):
    source = backend.wx_key(group, row)
    if receipts.is_echo(source) or receipts.is_delivered(source) or backend.wechat_system_message(row):
        return False
    locator = {key: row[key] for key in ('local_id', 'sort_seq', 'shard')}
    return receipts.db.execute('INSERT OR IGNORE INTO pending(source,group_id,locator) VALUES(?,?,?)',
        (source, group, json.dumps(locator))).rowcount > 0


def backfill_group(backend, access, receipts, group):
    """Opening a group adds its recent history without rewinding the live cursor."""
    if receipts.state('history_loaded:' + group, False):
        return 0
    recent = access.db.get_messages(group, limit=40)
    count = 0
    with receipts.db:
        for row in sorted(recent, key=lambda message: (message['sort_seq'], message['shard'], message['local_id'])):
            count += journal_row(backend, receipts, group, row)
        receipts.db.execute('INSERT OR REPLACE INTO state VALUES(?,?)', ('history_loaded:' + group, 'true'))
    return count


def watch_position(receipts, group):
    cursor = receipts.state('cursor:' + group, 0)
    return {'sort_seq': cursor - 1, 'shard': '', 'local_id': -1}


def journal_page(backend, access, receipts, group, position, limit=200):
    rows = access.db.get_new_message_page(group, position, limit=limit)
    cursor = receipts.state('cursor:' + group, 0)
    baseline = receipts.state('watch_floor:' + group)
    with receipts.db:
        for row in rows:
            if (baseline is None or int(row['sort_seq']) >= baseline) and not receipts.is_baseline(backend.wx_key(group, row)):
                journal_row(backend, receipts, group, row)
            cursor = max(cursor, int(row['sort_seq']))
        receipts.db.execute('INSERT OR REPLACE INTO state VALUES(?,?)', ('cursor:' + group, json.dumps(cursor)))
    if rows:
        position = {key: rows[-1][key] for key in ('sort_seq', 'shard', 'local_id')}
    return position, len(rows) == limit


def message_signature(access):
    result = []
    for path in sorted(Path(access.db.account_dir, 'db_storage', 'message').glob('*')):
        try:
            if path.is_file():
                info = path.stat()
                result.append((str(path), info.st_mtime_ns, info.st_size))
        except FileNotFoundError:
            continue  # A WAL rotation will be detected by the next signature.
    return tuple(result)


def authorize_sender(group, watched, active, connect_sender):
    if group not in watched:
        raise ValueError('该群未在本机服务端选择转发，不能发送')
    if group not in active:
        connect_sender(active | {group})
        active.add(group)


def forwarding_selection(value, catalog):
    try:
        value = json.loads(value) if isinstance(value, str) else value
    except (TypeError, ValueError) as exc:
        raise ValueError('转发群配置无效，需要群编号数组') from exc
    if (not isinstance(value, list) or len(value) > 4096 or
        any(not isinstance(group, str) or not 0 < len(group) <= 128 or any(ord(char) < 32 for char in group) for group in value)):
        raise ValueError('转发群配置无效，需要群编号数组')
    return set(value) & {group['id'] for group in catalog}


def restrict_native_groups(directory, allowed):
    """Shrink only this adapter's existing native allowlists, including when none are allowed."""
    directory = Path(directory).resolve()
    root = directory / 'native'
    if not root.resolve().is_relative_to(directory):
        raise ValueError('本机微信组件配置目录不在当前适配器目录内')
    configs = [root / 'native.json', *root.glob('*/native.json')]
    for config in configs:
        if not config.is_file():
            continue
        if not config.resolve().is_relative_to(root.resolve()):
            raise ValueError('本机微信组件配置不在当前适配器目录内')
        value = json.loads(config.read_text('utf-8'))
        if not isinstance(value, dict):
            raise ValueError('本机微信组件配置无效，无法确认授权已清空')
        groups = value.get('groups', [])
        if not isinstance(groups, list) or any(not isinstance(group, str) for group in groups):
            groups = []
        restricted = sorted(set(groups) & set(allowed))
        if value.get('groups') != restricted:
            value['groups'] = restricted
            temporary = config.with_suffix('.tmp')
            temporary.write_text(json.dumps(value, ensure_ascii=False), 'utf-8')
            temporary.replace(config)


def cancel_unselected(jobs, watched):
    kept, acknowledgements = [], []
    for job in jobs:
        result = job.cancel() if job.message['group'] not in watched else None
        if result is None:
            kept.append(job)
        else:
            acknowledgements.append(result)
    return kept, acknowledgements


def main():
    if sys.platform != 'win32':
        emit({'type': 'status', 'ready': False, 'text': '此适配器仅支持本机 Windows 微信'})
        return 2
    data = Path(os.environ.get('WECHAT_LITE_DATA', '.state')).resolve()
    directory = data / 'adapter'
    directory.mkdir(parents=True, exist_ok=True)
    if '--probe' not in sys.argv:
        restrict_native_groups(directory, set())
    backend = load_backend()
    with contextlib.redirect_stdout(sys.stderr):
        access = backend.WeChatAccess(directory / 'wechat')
        backend.main_process()  # Fingerprint verification only; no module is injected here.
        with access.db.snapshot_scope():
            groups = group_list(access)
    access.db.release()
    if '--probe' in sys.argv:
        emit({'ready': True, 'groups_count': len(groups), 'platform': 'windows', 'version': '4.1.15.13', 'sends': 0})
        access.db.release()
        return 0
    receipts = Receipts(directory / 'receipts.sqlite')
    access.confirmation_slice = 0.2
    native_groups = set()
    selected = forwarding_selection(os.environ.get('WECHAT_LITE_FORWARD_GROUPS', '[]'), groups)
    with contextlib.redirect_stdout(sys.stderr), access.db.snapshot_scope():
        signatures = message_signature(access)
        watched = initialize_watches(backend, access, receipts, [group for group in groups if group['id'] in selected])
    signatures = None  # Persisted cursors may have offline backlog even when the source files stay unchanged.

    def connect_sender(group_ids):
        nonlocal native_groups
        selected = set(group_ids)
        if not selected:
            restrict_native_groups(directory, set())
            native_groups = set()
            return
        if selected == native_groups and access.native:
            return
        if selected <= native_groups and access.native:
            restrict_native_groups(directory, selected)
            native_groups = selected
            return
        native = backend.NativeClient(directory / 'native', VENDOR / 'native' / 'WeChatLocal.dll')
        health = native.connect(selected, directory / 'feishu' / 'media', disable_older=False)
        if not {'text', 'mentions', 'image', 'gif', 'file', 'video_download'} <= set(health.get('capabilities', [])):
            raise RuntimeError('本机微信组件功能不完整，请更新组件')
        access.native = native
        native_groups = selected

    emit({'type': 'groups', 'groups': groups})
    emit({'type': 'status', 'ready': True, 'text': f'已连接本机 Windows 微信 · 已选择 {len(watched)} 个群转发'})
    active, pending_acks = set(), {}
    pages, jobs = {}, []

    def set_forwarding(next_selected, catalog):
        nonlocal selected, watched, jobs, pages, signatures
        allowed = set(next_selected) & {group['id'] for group in catalog}
        changed = allowed != watched
        # Revoke first: a failure to baseline a newly selected group cannot keep an old group enabled.
        watched.intersection_update(allowed)
        active.intersection_update(allowed)
        jobs, acknowledgements = cancel_unselected(jobs, allowed)
        for acknowledgement in acknowledgements:
            emit(acknowledgement)
        connect_sender(active)
        selected = set(next_selected)
        pages = {group: position for group, position in pages.items() if group in allowed}
        if changed:
            signatures = None
        watched = initialize_watches(backend, access, receipts, [group for group in catalog if group['id'] in allowed])

    commands = queue.Queue(maxsize=128)
    def read_commands():
        for line in sys.stdin:
            try:
                if len(line) > 2 * 1024 * 1024:
                    raise ValueError('命令过大')
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise ValueError('命令必须是对象')
                commands.put(value)
            except (ValueError, json.JSONDecodeError):
                emit({'type': 'warning', 'text': '忽略无效的适配器命令'})
        commands.put({'op': 'quit'})
    threading.Thread(target=read_commands, daemon=True).start()
    last_poll, last_members, last_media, last_send = 0.0, 0.0, 0.0, 0.0
    last_groups = time.monotonic()
    try:
        while True:
            try:
                command = commands.get(timeout=0.7)
            except queue.Empty:
                command = {}
            operation = command.get('op')
            if operation == 'quit':
                break
            try:
                poll_due = bool(watched) and time.monotonic() - last_poll >= 1.4
                # Capture before any command reads the shared snapshot; a later WAL write must trigger another round.
                current = message_signature(access) if poll_due else None
                with contextlib.redirect_stdout(sys.stderr), access.db.snapshot_scope():
                    if operation == 'activate':
                        group = command['group']
                        # Opening/sending explicitly grants native access; watching a group never does.
                        authorize_sender(group, watched, active, connect_sender)
                        backfill_group(backend, access, receipts, group)
                        emit({'type': 'members', 'group': group, 'members': member_list(access, group)})
                    elif operation == 'configure':
                        if not isinstance(command.get('groups'), list):
                            raise ValueError('转发群配置无效，需要群编号数组')
                        set_forwarding(forwarding_selection(command.get('groups'), groups), groups)
                        emit({'type': 'status', 'ready': True, 'text': f'已连接本机 Windows 微信 · 已选择 {len(watched)} 个群转发'})
                    elif operation == 'receipt':
                        source = command['source_id']
                        receipts.acknowledge(command['group'], source)
                        pending_acks.pop(source, None)
                    elif operation == 'send':
                        message = command['message']
                        if not any(job.message['id'] == message['id'] for job in jobs):
                            if len(jobs) >= 128:
                                raise ValueError('微信发送队列已满，请稍后重试')
                            authorize_sender(message['group'], watched, active, connect_sender)
                            jobs.append(OutgoingMessage(backend, access, receipts, data, directory, active, message))
                    now = time.monotonic()
                    if jobs and now - last_send >= 1.4:
                        last_send = now
                        job = jobs[0]
                        result = job.cancel() if job.message['group'] not in watched else None
                        if result is None:
                            result = job.advance()
                        if result:
                            emit(result)
                            jobs.pop(0)
                    if now - last_groups >= 60:
                        refreshed = group_list(access)
                        set_forwarding(selected, refreshed)
                        if refreshed != groups:
                            groups = refreshed
                            emit({'type': 'groups', 'groups': groups})
                        last_groups = now
                    if poll_due:
                        last_poll = now
                        if current != signatures:
                            more = False
                            for group in sorted(watched):
                                cursor = pages.get(group) or watch_position(receipts, group)
                                pages[group], remaining = journal_page(backend, access, receipts, group, cursor)
                                more |= remaining
                            if not more:
                                signatures = current
                                pages.clear()
                    if active and now - last_members > 60:
                        for group in sorted(active):
                            emit({'type': 'members', 'group': group, 'members': member_list(access, group)})
                        last_members = now
                    if now - last_media > 1.4:
                        last_media = now
                        placeholders = ','.join('?' for _ in watched)
                        rows = receipts.db.execute('SELECT source,group_id,locator FROM pending WHERE due<? AND group_id IN (' + placeholders + ') ORDER BY due,rowid LIMIT 8', (time.time(), *watched)).fetchall() if watched else []
                        for source, group, locator in rows:
                            if time.monotonic() - pending_acks.get(source, -float('inf')) < 30:
                                continue
                            if receipts.is_echo(source):
                                with receipts.db:
                                    receipts.db.execute('DELETE FROM pending WHERE source=?', (source,))
                                continue
                            try:
                                original = access.db.reload_message(group, json.loads(locator))
                                if jobs and jobs[0].defers(group, original):
                                    with receipts.db:
                                        receipts.db.execute('UPDATE pending SET due=? WHERE source=?', (time.time() + 2, source))
                                    continue
                                converted = convert_message(backend, access, group, original)
                                if converted:
                                    emit({'type': 'message', 'message': converted})
                                    pending_acks[source] = time.monotonic()
                                    with receipts.db:
                                        receipts.db.execute('UPDATE pending SET due=? WHERE source=?', (time.time() + 30, source))
                                else:
                                    with receipts.db:
                                        receipts.db.execute('DELETE FROM pending WHERE source=?', (source,))
                            except Exception as exc:
                                with receipts.db:
                                    receipts.db.execute('UPDATE pending SET due=? WHERE source=?', (time.time() + 30, source))
                                if source not in receipts.warned:
                                    receipts.warned.add(source)
                                    emit({'type': 'warning', 'group': group, 'text': '微信附件暂未就绪，将保留并重试：' + str(exc)})
            except Exception as exc:
                if operation == 'send':
                    message = command.get('message')
                    ident = message.get('id', '') if isinstance(message, dict) else ''
                    emit({'type': 'ack', 'id': ident, 'status': 'uncertain' if receipts.submitted(ident) else 'failed', 'error': str(exc)})
                else:
                    warning = {'type': 'warning', 'text': str(exc)}
                    if operation == 'activate' and isinstance(command.get('group'), str):
                        warning['group'] = command['group']
                    emit(warning)
    finally:
        try:
            restrict_native_groups(directory, set())
        finally:
            access.db.release()
            receipts.db.close()
    return 0


if __name__ == '__main__':
    configure_stdio()
    try:
        raise SystemExit(main())
    except Exception as exc:
        emit({'type': 'status', 'ready': False, 'text': str(exc)})
        raise SystemExit(1)

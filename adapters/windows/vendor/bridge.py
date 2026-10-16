"""A local, explicitly selected WeChat/Feishu group bridge. No sends on import."""
from __future__ import annotations

from contextlib import nullcontext

import hashlib
import bisect
import io
import json
import os
from pathlib import Path
import queue
import re
import shutil
import sqlite3
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

MAX_FILE = 512 * 1024 * 1024
MAX_POST = 28 * 1024
MAX_IMAGE = 10 * 1024 * 1024
ALERT_NAMES = ('温家俊', '温小虎', '家俊', '小虎')


class Unavailable(RuntimeError):
    """Nothing was sent; the source or destination is not ready."""


class MediaPending(Unavailable):
    """The source attachment has not arrived in the local cache yet."""


class ConfirmationPending(Unavailable):
    """The native request is submitted; only its acknowledgement may be polled."""


class Uncertain(RuntimeError):
    """A send was attempted but not acknowledged. Never retry automatically."""


class PermissionDenied(Unavailable):
    """Pause the relay instead of rejecting every queued message."""


def xml_root(content):
    if isinstance(content, bytes):
        if len(content) > 8_000_000:
            raise Unavailable('消息 XML 超出安全解析范围')
        try:
            if content.startswith(b'\x28\xb5\x2f\xfd'):
                import zstandard
                if zstandard.frame_content_size(content) > 8_000_000:
                    raise Unavailable('消息 XML 超出安全解析范围')
                content = zstandard.ZstdDecompressor().decompress(content, max_output_size=8_000_000)
            content = content.decode('utf-8')
        except Exception as exc:
            raise Unavailable('消息 XML 无法完整解码') from exc
    text = str(content or "")
    start = text.find("<")
    if start < 0:
        return None
    text = text[start:]
    if len(text) > 8_000_000 or "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        raise Unavailable("消息 XML 超出安全解析范围")
    try:
        return ET.fromstring(text)
    except ET.ParseError as exc:
        raise Unavailable("消息 XML 无法完整解析") from exc


def text_at(root, *names):
    for name in names:
        element = root.find(".//" + name)
        if element is not None and element.text:
            return element.text
    return ""


def emoji_url(url):
    try:
        parsed = urllib.parse.urlsplit(url)
        valid = (parsed.scheme in ('http', 'https') and parsed.hostname in (
            'vweixinf.tc.qq.com', 'wxapp.tc.qq.com', 'mmbiz.qpic.cn', 'mmbiz.qlogo.cn')
            and not parsed.username and not parsed.password and not parsed.fragment
            and parsed.port in (None, 80 if parsed.scheme == 'http' else 443))
    except ValueError:
        valid = False
    if not valid:
        raise Unavailable('微信表情资源地址不在允许的腾讯 CDN 范围内')
    return url


def emoji_resource(url):
    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return super().redirect_request(req, fp, code, msg, headers, emoji_url(newurl))
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), Redirect())
    try:
        with opener.open(emoji_url(url), timeout=12) as response:
            emoji_url(response.geturl())
            data = response.read(MAX_IMAGE + 1)
    except (urllib.error.URLError, TimeoutError, OSError):
        raise MediaPending('微信表情资源暂时无法下载，恢复后会自动补发') from None
    if len(data) > MAX_IMAGE:
        raise Unavailable('微信表情超过飞书图片 10 MB 上传上限')
    return data


def display_name(value, identifier=''):
    if not isinstance(value, str) or value.strip().rstrip(':：') == identifier or re.fullmatch(
            r'(?:ou_|on_|cli_|oc_|om_|wxid_)[A-Za-z0-9_-]+[:：]?', value.strip()):
        return ''
    return value.strip()


def quote_text(quote, depth=0):
    if depth > 8:
        return '[嵌套引用]'
    kind = text_at(quote, 'type')
    content = quote.findtext('./content') or ''
    if kind == '1':
        return content
    label = {'3': '[图片]', '34': '[语音]', '43': '[视频]', '47': '[表情]',
             '42': '[名片]', '48': '[位置]'}
    if kind in label:
        return label[kind]
    try:
        root = xml_root(content)
    except Unavailable:
        return '[引用消息无法预览]'
    if root is None:
        return content or '[引用消息]'
    if root.tag == 'img' or root.find('.//img') is not None:
        return '[图片]'
    app = root if root.tag == 'appmsg' else root.find('.//appmsg')
    if app is not None:
        app_type = app.findtext('./type') or ''
        title = app.findtext('./title') or ''
        if app_type == '57':
            nested = app.find('./refermsg')
            if nested is not None:
                title += '\n引用 ' + (display_name(text_at(nested, 'displayname'), text_at(nested, 'fromusr')) or '群成员') + '：' + quote_text(nested, depth + 1)
            return title.strip() or '[引用消息]'
        if app_type == '5':
            return '[链接]' + ' '.join(filter(None, [title, app.findtext('./url')]))
        return {'6': '[文件]', '74': '[文件]', '19': '[聊天记录]', '33': '[小程序]', '36': '[小程序]', '51': '[视频号]'}.get(
            app_type, '[消息卡片]') + title
    return '[引用消息无法预览]'


def wechat_reply_id(payload):
    if payload.get('type') == '文本' or '<refermsg' not in str(payload.get('content', '')):
        return ''
    root = xml_root(payload['content'])
    app = root.find('.//appmsg') if root is not None else None
    if app is None or app.findtext('./type') != '57':
        return ''
    source = app.findtext('./refermsg/svrid') or ''
    return str(int(source) % (1 << 64)) if re.fullmatch(r'-?\d{1,20}', source) and int(source) else ''


def record_items(root, depth=0):
    if depth > 8:
        raise Unavailable("合并转发嵌套超过 8 层")
    record = root.find(".//recorditem")
    if record is not None and record.text:
        nested = xml_root(record.text)
        if nested is not None:
            root = nested
    datalist = root.find(".//datalist")
    entries = datalist.findall("./dataitem") if datalist is not None else []
    if not entries:
        raise Unavailable("合并转发记录未加载，不能用卡片标题代替内部内容")
    if len(entries) > 2000:
        raise Unavailable("合并转发包含超过 2000 条记录")
    result = []
    for item in entries:
        kind = int(item.get("datatype", "0"))
        sender = display_name(text_at(item, 'sourcename'), text_at(item, 'fromusr', 'sourceusername')) or '群成员'
        if kind == 17:
            result.extend(record_items(item, depth + 1))
        elif kind in (1, 5, 6, 7):
            body = " ".join(dict.fromkeys(filter(None, [
                text_at(item, "datatitle"), text_at(item, "datadesc"),
                text_at(item, "link", "weburl", "url"),
            ])))
            if not body:
                raise Unavailable("合并转发的一条记录没有可读正文")
            result.append({"sender": sender, "sender_username": text_at(item, 'sourceusername', 'fromusr'), "kind": "text", "text": body})
        elif kind in (2, 3, 4, 8):
            result.append({"sender": sender, "sender_username": text_at(item, 'sourceusername', 'fromusr'), "kind": {2: "image", 3: "record", 4: "video", 8: "file"}[kind],
                           "md5": text_at(item, "datamd5", "fullmd5"),
                           "name": text_at(item, "datatitle"), "size": text_at(item, "datasize")})
        else:
            raise Unavailable(f"合并转发内类型 {kind} 暂无完整转换方式")
    return result


def wx_key(group, message):
    return f"wx:{group}:{message.get('shard', '')}:{message['local_id']}:{message['sort_seq']}"


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def wechat_system_message(message):
    kind = str(message.get('type', ''))
    if kind.startswith('系统'):
        return True
    if kind != '文件/链接/卡片':
        return False
    try:
        root = xml_root(message.get('content', ''))
        # 微信把“拍一拍”也存为 appmsg，而不是普通的系统消息类型。
        return root is not None and root.tag == 'msg' and root.findtext('./appmsg/type') == '62'
    except Unavailable:
        return False


def card_source_matches(group, message, ops, expected):
    if fingerprint(ops) == expected:
        return True
    if message.get('type') != '图片':
        return False
    candidate = json.loads(json.dumps(ops))
    images = [node for op in candidate for line in op.get('paragraphs', []) for node in line
              if node.get('tag') == 'img' and node.get('path')]
    if len(images) != 1:
        return False
    path = Path(images[0]['path'])
    stem = f'{group}_{message["local_id"]}'
    if path.stem not in (stem, stem + '_h'):
        return False
    # 同一条图片的压缩缓存和原图缓存只差 _h，不能把缓存档位变化当作消息变更。
    # 目录、扩展名、正文和发送者仍参与完整指纹核对，实际发送使用当前可用的完整图片。
    images[0]['path'] = str(path.with_name((stem if path.stem.endswith('_h') else stem + '_h') + path.suffix))
    return fingerprint(candidate) == expected


def media_paths(value):
    if isinstance(value, dict):
        if isinstance(value.get('path'), str):
            yield value['path']
        for item in value.values():
            yield from media_paths(item)
    elif isinstance(value, list):
        for item in value:
            yield from media_paths(item)


def split_text_ops(ops):
    result = []
    for op in ops:
        if op["kind"] == "text":
            start = 0
            names = op.get('atnames', {})
            while start < len(op['text']):
                end = min(start + 1500, len(op['text']))
                for token in names.values():
                    position = op['text'].rfind(token, max(start, end - len(token) + 1), min(len(op['text']), end + len(token)))
                    if position >= start and position < end < position + len(token):
                        end = position
                piece = dict(op, text=op['text'][start:end])
                if names:
                    piece['atlist'] = ','.join(ident for ident, token in names.items() if token in piece['text'])
                result.append(piece)
                start = end
        else:
            result.append(op)
    return result


def post_size(paragraphs):
    # Leave room for uploaded image keys and the outer request within Feishu's 30 KB limit.
    content = {'zh_cn': {'content': [[dict(tag='img', image_key='x' * 256) if node['tag'] == 'img'
                                    else node for node in line] for line in paragraphs]}}
    return len(json.dumps({'content': json.dumps(content, ensure_ascii=False)}, ensure_ascii=False).encode('utf-8')) + 512


def post_ops(ops):
    result = []
    lines = [[{'tag': 'at', 'user_id': 'all'}]] if any(op.get('mention_all') for op in ops) else []
    for op in ops:
        if op['kind'] not in ('text', 'image'):
            if lines:
                result.append({'kind': 'post', 'paragraphs': lines})
                lines = []
            result.append(op)
            continue
        nodes = [dict(tag='text', text=piece['text']) for piece in split_text_ops([op])] if op['kind'] == 'text' else [dict(tag='img', path=op['path'])]
        if nodes and op.get('sender'):
            nodes[0]['sender'] = op['sender']
        line = lines.pop() + [{'tag': 'text', 'text': ' '}] if lines == [[{'tag': 'at', 'user_id': 'all'}]] else []
        for node in nodes:
            if post_size(lines + [line + [node]]) > MAX_POST:
                if line:
                    lines.append(line)
                if lines:
                    result.append({'kind': 'post', 'paragraphs': lines})
                lines, line = [], []
            if post_size([[node]]) > MAX_POST:
                raise Unavailable('一段微信内容超出飞书富文本容量，已保留原消息')
            line.append(node)
        lines.append(line)
    if lines:
        result.append({'kind': 'post', 'paragraphs': lines})
    return result


def hour_paragraphs(ops):
    if not ops or ops[0]['kind'] != 'post' or ops[0].get('reply_to'):
        return None
    if len(ops) == 1:
        return ops[0]['paragraphs']
    if len(ops) != 2 or ops[1]['kind'] not in ('file', 'video'):
        return None
    path = Path(ops[1]['path'])
    if not path.is_file() or not 0 < path.stat().st_size <= MAX_FILE:
        raise Unavailable('小时卡片的附件原件不完整，未发送')
    video = ops[1]['kind'] == 'video'
    if path.suffix.lower() == '.mp4':
        with path.open('rb') as stream:
            video = stream.read(12)[4:8] == b'ftyp'
    lines = [[dict(node) for node in line] for line in ops[0]['paragraphs']]
    for line in lines:
        if len(line) == 1 and line[0].get('sender'):
            node = line[0]
            header = node['sender'] + '：'
            if node.get('text') == header + path.name:
                node['text'] = header
    return lines + [[{'tag': 'attachment', 'name': path.name, 'size': path.stat().st_size,
                     'media_type': 'video' if video else 'file', 'path': str(path)}]]


class Store:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        # 语句缓存会保留上次绑定的正文参数，完成后也必须释放它们。
        self.conn = sqlite3.connect(':memory:', cached_statements=0)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute('PRAGMA temp_store=MEMORY')
        self.conn.execute('PRAGMA secure_delete=ON')
        self.conn.execute('ATTACH DATABASE ? AS saved', (str(self.directory / 'queue.sqlite3'),))
        self.conn.execute('PRAGMA saved.secure_delete=ON')
        legacy = self.conn.execute("SELECT 1 FROM saved.sqlite_master WHERE type='table' AND name='task'").fetchone()
        if legacy:
            self.conn.execute('ALTER TABLE saved.task RENAME TO receipt')
        self.conn.executescript("""
            CREATE TABLE IF NOT EXISTS saved.receipt (
                id INTEGER PRIMARY KEY, pair TEXT, key TEXT, direction TEXT, payload TEXT,
                ops TEXT, step INTEGER DEFAULT 0, status TEXT DEFAULT 'pending', error TEXT DEFAULT '',
                UNIQUE(pair,key));
            CREATE TABLE IF NOT EXISTS saved.state (key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE IF NOT EXISTS saved.echo (key TEXT PRIMARY KEY);
            CREATE TABLE IF NOT EXISTS saved.temporary (task_id INTEGER, path TEXT, PRIMARY KEY(task_id,path));
            UPDATE receipt SET status='uncertain',error='上次运行在发送过程中退出，请先检查目标群'
                WHERE status='sending';
        """)
        if 'batch' not in {row['name'] for row in self.conn.execute('PRAGMA saved.table_info(receipt)')}:
            self.conn.execute('ALTER TABLE receipt ADD COLUMN batch INTEGER')
            self.conn.commit()
        with self.conn:
            self.conn.execute("""UPDATE receipt SET status='ignored',error='' WHERE direction='wx_to_fs'
                AND step=0 AND status IN ('pending','failed')
                AND json_extract(CASE WHEN json_valid(payload) THEN payload ELSE '{}' END,'$.type') LIKE '系统%'""")
            self.conn.execute("""INSERT OR REPLACE INTO state(key,value)
                SELECT 'media_wait:' || id,'true' FROM receipt WHERE direction='wx_to_fs' AND status='failed'
                AND ((error='微信媒体尚未在本机完整下载，请在微信打开该消息后重试'
                    AND json_extract(CASE WHEN json_valid(payload) THEN payload ELSE '{}' END,'$.type') IN ('图片','视频'))
                OR (step=0 AND ((error='error：处理未完成'
                    AND json_extract(CASE WHEN json_valid(payload) THEN payload ELSE '{}' END,'$.type')='图片')
                    OR (error='微信视频缺少有效的大小或 MD5，不能确认完整原文件'
                    AND json_extract(CASE WHEN json_valid(payload) THEN payload ELSE '{}' END,'$.type')='视频'))))""")
            for row in self.conn.execute('SELECT id,direction,status,payload,ops FROM receipt').fetchall():
                if legacy and row['ops']:
                    self.register_files(row['id'], json.loads(row['ops']))
                payload = self.locator(row['payload'], row['direction'], row['status'])
                self.conn.execute('UPDATE receipt SET payload=?,ops=NULL WHERE id=?', (payload, row['id']))
            for row in self.conn.execute('SELECT key,value FROM state').fetchall():
                if self.safe_state_key(row['key']):
                    value = self.safe_state(row['key'], json.loads(row['value']))
                    self.conn.execute('UPDATE state SET value=? WHERE key=?', (json.dumps(value), row['key']))
                else:
                    self.conn.execute('DELETE FROM state WHERE key=?', (row['key'],))
        # 旧版正文可能留在 SQLite 空闲页中，迁移后压缩，而不是另存备份。
        if legacy:
            self.conn.execute('VACUUM saved')
        self.conn.executescript("""
            CREATE TABLE task (
                id INTEGER PRIMARY KEY, pair TEXT, key TEXT, direction TEXT, payload TEXT,
                ops TEXT, step INTEGER DEFAULT 0, status TEXT DEFAULT 'pending', error TEXT DEFAULT '',
                batch INTEGER, UNIQUE(pair,key));
            INSERT INTO task SELECT * FROM receipt;
        """)
        self.conn.create_function('source_locator', 3, self.locator)
        self.conn.executescript("""
            CREATE TEMP TRIGGER save_insert AFTER INSERT ON main.task BEGIN
                INSERT INTO receipt VALUES(new.id,new.pair,new.key,new.direction,
                    source_locator(new.payload,new.direction,new.status),NULL,new.step,new.status,new.error,new.batch);
            END;
            CREATE TEMP TRIGGER save_update AFTER UPDATE ON main.task BEGIN
                UPDATE receipt SET payload=source_locator(new.payload,new.direction,new.status),ops=NULL,
                    step=new.step,status=new.status,error=new.error,batch=new.batch WHERE id=new.id;
            END;
        """)
        self.cleanup_completed_files()

    def register_files(self, ident, value):
        for path in media_paths(value):
            source = Path(path).resolve()
            roots = self.media_roots()
            if not any(source.is_relative_to(root) and source.resolve().is_relative_to(root) and source != root for root in roots):
                continue
            self.conn.execute('INSERT OR IGNORE INTO temporary VALUES (?,?)', (ident, str(source)))
        self.conn.commit()

    def media_roots(self):
        return tuple(path for path in (self.directory / 'wechat/media', self.directory / 'feishu/media')
                     if path.resolve() == path)

    def cleanup_completed_files(self):
        rows = self.conn.execute("""SELECT t.task_id,t.path FROM temporary t JOIN receipt r ON r.id=t.task_id
            WHERE r.status IN ('done','ignored')""").fetchall()
        roots = self.media_roots()
        for row in rows:
            in_use = self.conn.execute("""SELECT 1 FROM temporary t JOIN receipt r ON r.id=t.task_id
                WHERE path=? AND r.status NOT IN ('done','ignored') LIMIT 1""", (row['path'],)).fetchone()
            path = Path(row['path']).absolute()
            root = next((root for root in roots if path.is_relative_to(root) and path.resolve().is_relative_to(root) and path != root), None)
            if not in_use and root:
                try:
                    path.unlink(missing_ok=True)
                    parent = path.parent
                    while parent != root:
                        try:
                            parent.rmdir()
                        except OSError:
                            break
                        parent = parent.parent
                except OSError:
                    continue
            self.conn.execute('DELETE FROM temporary WHERE task_id=? AND path=?', (row['task_id'], row['path']))
        self.conn.commit()

    @staticmethod
    def locator(payload, direction, status):
        if status in ('done', 'ignored'):
            return '{}'
        value = json.loads(payload or '{}')
        names = ('local_id', 'sort_seq', 'shard', 'type') if direction == 'wx_to_fs' else (
            ('message_id', 'form_nonce') if direction in ('fs_to_wx', 'card_to_wx') else ('source_count',))
        return json.dumps(dict({key: value[key] for key in names if key in value}, _source_only=True), ensure_ascii=False)

    @staticmethod
    def safe_state_key(key):
        return bool(re.fullmatch(r'(?:wx_attempt:.+|wx_retry:\d+|media_wait:\d+|wx_source:\d+|sent:\d+:\d+|all_notice:\d+|member_notice:\d+|card_merge:\d+|video_post:\d+|hour_media:\d+|cloud_media:\d+|fs_owner:[a-f0-9]{64}:om_[A-Za-z0-9_-]+|recall_source:[a-f0-9]{64}|'
                                 r'recall_attempt:[a-f0-9]{64}|reply_source:[a-f0-9]{64}|reply_form:[a-f0-9]{32}|'
                                 r'fs_cleanup:[a-f0-9]{64}:om_[A-Za-z0-9_-]+|echo_asset:[a-f0-9]{64}:[a-f0-9]{64}|'
                                 r'composer_ready:cli_[A-Za-z0-9]+|plan:\d+|[a-f0-9]{64}:(?:moderation|composer_entry|upload_folder|drive_folder|drive_file:[a-f0-9]{64}|(?:hour_card|day_card)(?::-?\d+:\d+)?|day_slot:-?\d+(?::\d+)?|fs_floor|fs_time|wx_cursor|wx_history_(?:done|boundary|cursor)|wx_sent:\d+))', key))

    @staticmethod
    def safe_state(key, value):
        if key.startswith('fs_cleanup:'):
            return {name: value[name] for name in ('chat', 'owner', 'origin', 'hash', 'count', 'phase', 'error', 'time') if name in value}
        if key.startswith('echo_asset:'):
            return {name: value[name] for name in ('kind', 'name', 'size', 'md5', 'wx_hash', 'img_key', 'cover_key', 'url') if name in value}
        if key.startswith('reply_source:'):
            return {name: value[name] for name in ('pair', 'chat', 'member', 'local_id', 'sort_seq', 'shard', 'time') if name in value}
        if key.startswith('reply_form:'):
            return {name: value[name] for name in ('chat', 'owner', 'source', 'phase', 'message_id', 'task_id', 'count', 'prefix_hash', 'prefix_length') if name in value}
        if key.startswith('fs_owner:'):
            return {'owner': value['owner']}
        if key.startswith('recall_source:'):
            return {name: value[name] for name in ('pair', 'chat', 'owner', 'origin', 'ack', 'time', 'local_id', 'sort_seq', 'shard')}
        if key.startswith('recall_attempt:'):
            return {name: value[name] for name in ('server_id', 'phase', 'time') if name in value}
        if key.startswith('wx_attempt:'):
            value = dict(value)
            expected = value.pop('expected', None)
            if expected is not None:
                value['expected_hash'] = fingerprint(expected)
            response = value.get('response')
            if isinstance(response, dict):
                value['response'] = {name: response[name] for name in ('ret', 'status', 'uuid', 'request_id') if name in response}
        return value

    def state(self, key, default=None):
        row = self.conn.execute("SELECT value FROM state WHERE key=?", (key,)).fetchone()
        return json.loads(row[0]) if row else default

    def set_state(self, key, value):
        if not self.safe_state_key(key):
            raise ValueError('只能持久化同步游标、消息 ID 和去重指纹')
        value = self.safe_state(key, value)
        self.conn.execute("INSERT OR REPLACE INTO state VALUES (?,?)", (key, json.dumps(value)))
        self.conn.commit()

    def enqueue(self, pair, key, direction, payload):
        status = 'ignored' if direction == 'wx_to_fs' and wechat_system_message(payload) else 'pending'
        cursor = self.conn.execute("INSERT OR IGNORE INTO task(pair,key,direction,payload,status) VALUES (?,?,?,?,?)",
                                   (pair, key, direction, json.dumps({'type': '系统消息'} if status == 'ignored' else payload, ensure_ascii=False), status))
        self.conn.commit()
        if not cursor.rowcount:
            row = self.conn.execute('SELECT id,status,payload FROM task WHERE pair=? AND key=?', (pair, key)).fetchone()
            if row['status'] not in ('done', 'ignored') and json.loads(row['payload']).get('_source_only'):
                self.update(row['id'], payload=json.dumps(payload, ensure_ascii=False))
        return bool(cursor.rowcount)

    def add_echo(self, key):
        self.conn.execute("INSERT OR IGNORE INTO echo VALUES (?)", (key,))
        self.conn.commit()

    def is_echo(self, key):
        return bool(self.conn.execute("SELECT 1 FROM echo WHERE key=?", (key,)).fetchone())

    def native_echo(self, pair, key):
        key = key.removeprefix('wx_card:')
        return bool(self.conn.execute("""SELECT 1 FROM state WHERE key LIKE ?
            AND json_extract(value,'$.ack')=? AND json_extract(value,'$.server_id') IS NOT NULL LIMIT 1""",
            ('wx_attempt:' + pair + ':%', key)).fetchone())

    def native_origin(self, pair, key):
        key = key.removeprefix('wx_card:')
        prefix = 'wx_attempt:' + pair + ':fs:'
        rows = self.conn.execute("""SELECT key FROM state WHERE key LIKE ? AND json_extract(value,'$.ack')=?
            AND json_extract(value,'$.server_id') IS NOT NULL""", (prefix + '%', key)).fetchall()
        origins = set()
        for row in rows:
            match = re.fullmatch(r'(om_[A-Za-z0-9_-]+):(\d+):\d+', row[0][len(prefix):])
            if match:
                origins.add((match[1], int(match[2])))
        if len(origins) != 1:
            raise Unavailable('附件无法唯一对应到已送达的飞书原消息，未补入卡片')
        return next(iter(origins))

    def next_task(self, pair, excluded=()):
        placeholders = ','.join('?' for _ in excluded)
        exclusion = ' AND t.id NOT IN (' + placeholders + ')' if excluded else ''
        return self.conn.execute("""SELECT * FROM task t WHERE pair=? AND status='pending'""" + exclusion + """
            ORDER BY CASE WHEN direction IN ('fs_to_wx','card_to_wx') THEN 0
                WHEN direction='wx_to_fs' AND EXISTS (SELECT 1 FROM state WHERE key LIKE 'wx_attempt:' || t.pair || ':%'
                    AND json_extract(value,'$.ack')=CASE WHEN t.key LIKE 'wx_card:%' THEN substr(t.key,9) ELSE t.key END
                    AND json_extract(value,'$.server_id') IS NOT NULL) THEN 1 ELSE 2 END,
            COALESCE((SELECT min(s.id) FROM task s WHERE s.batch=t.id),t.id) LIMIT 1""", (pair, *excluded)).fetchone()

    def update(self, ident, **fields):
        assert set(fields) <= {"status", "error", "ops", "step", "payload"}
        self.conn.execute("UPDATE task SET " + ",".join(k + "=?" for k in fields) + " WHERE id=?",
                          (*fields.values(), ident))
        self.conn.commit()

    def batch_tasks(self, tasks, op):
        pair = tasks[0]['pair']
        key = 'wx_batch:' + hashlib.sha256('\n'.join(task['key'] for task in tasks).encode()).hexdigest()
        with self.conn:
            cursor = self.conn.execute('INSERT INTO task(pair,key,direction,payload,ops) VALUES (?,?,?,?,?)',
                (pair, key, 'wx_batch_to_fs', json.dumps({'source_count': len(tasks)}), json.dumps([op], ensure_ascii=False)))
            ident = cursor.lastrowid
            for task in tasks:
                changed = self.conn.execute("UPDATE task SET status='batched',batch=? WHERE id=? AND pair=? AND status='pending' AND step=0",
                                            (ident, task['id'], pair))
                if changed.rowcount != 1:
                    raise Unavailable('历史批次的源消息状态发生变化，未发送')
        return self.conn.execute('SELECT * FROM task WHERE id=?', (ident,)).fetchone()

    def complete(self, ident):
        with self.conn:
            self.conn.execute("UPDATE task SET status='done',error='',payload='{}',ops=NULL WHERE id=? OR batch=?", (ident, ident))
            self.conn.execute('DELETE FROM state WHERE key IN (?,?,?,?,?)', (f'card_merge:{ident}', f'member_notice:{ident}', f'video_post:{ident}', f'hour_media:{ident}', f'cloud_media:{ident}'))
        self.cleanup_completed_files()

    def retry(self, ident):
        row = self.conn.execute('SELECT status,direction FROM task WHERE id=?', (ident,)).fetchone()
        if row and row['status'] == 'uncertain' and row['direction'] == 'fs_to_wx':
            key = f'wx_retry:{ident}'
            self.set_state(key, self.state(key, 0) + 1)
        self.update(ident, status='pending', error='')

    def retry_media(self, pair):
        with self.conn:
            self.conn.execute("""UPDATE task SET status='pending',payload=source_locator(payload,direction,status)
                WHERE pair=? AND direction='wx_to_fs'
                AND status='failed' AND EXISTS (SELECT 1 FROM state
                    WHERE key='media_wait:' || task.id AND value='true')""", (pair,))

    def counts(self, pair):
        return dict(self.conn.execute("""SELECT CASE WHEN source.status='batched' THEN parent.status ELSE source.status END AS progress,count(*)
            FROM task source LEFT JOIN task parent ON source.batch=parent.id
            WHERE source.pair=? AND source.direction!='wx_batch_to_fs' GROUP BY progress""", (pair,)))

    def failed(self, pair):
        return [dict(row) for row in self.conn.execute(
            "SELECT id,direction,status,error FROM task WHERE pair=? AND status IN ('failed','uncertain') ORDER BY id", (pair,))]


class WeChatAccess:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        from wechatauto import MediaDownloader, WxParam, wxlog
        from memory_db import MemoryWeChatDB
        # 正文只驻留内存，依赖的调试日志也不能保存正文。
        WxParam.ENABLE_FILE_LOGGER = False
        wxlog.logger.disabled = True
        self.db = MemoryWeChatDB(workdir=str(self.directory / "db"))
        for path in Path(self.db.workdir).iterdir():
            if '.db' in path.name and path.is_file() and not path.is_symlink():
                path.unlink()
        self.media_dir = self.directory / "media"
        self.media_dir.mkdir(exist_ok=True)
        self.media = MediaDownloader(self.db, save_dir=str(self.media_dir))
        self._write_original_media = self.media._write_decrypted
        self.media._write_decrypted = self.write_decrypted_media
        self.native = None
        self.recaller = None

    def write_decrypted_media(self, data, stem, save_dir):
        if not data.startswith(b'wxgf'):
            return self._write_original_media(data, stem, save_dir)
        from wxgf import decode_wxgf
        picture = decode_wxgf(data)
        path = self.media._out(save_dir, stem + '.png')
        Path(path).write_bytes(picture)
        return path

    def connect_sender(self, groups):
        import sys
        from native_sender import NativeClient
        bundled = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'native/WeChatLocal.dll'
        native = NativeClient(self.directory.parent / 'native', bundled)
        try:
            health = native.connect(groups, self.directory.parent / 'feishu/media')
        except Exception as exc:
            raise Unavailable('微信后台发送组件未连接：' + str(exc)) from exc
        if not {'text', 'mentions', 'image', 'gif', 'file', 'video_download'} <= set(health.get('capabilities', [])):
            raise Unavailable('微信后台发送组件需要更新，消息已保留')
        self.native = native

    def connect_recaller(self, groups):
        import sys
        from native_recall import NativeRecallClient, TEST_GROUP, TEST_SELF
        if TEST_GROUP not in groups or self.db.wxid != TEST_SELF:
            return False
        bundled = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'native/WeChatRecallTest.dll'
        caller = NativeRecallClient(self.directory.parent / 'native_recall_test', bundled)
        caller.connect([TEST_GROUP], self.directory.parent / 'feishu/media')
        self.recaller = caller
        return True

    def recalled(self, group, source):
        message = self.db.reload_message(group, source)
        if self.server_id(group, message) != str(source['server_id']):
            return False
        body = self.message_identity(group, message)[0]
        root = xml_root(body)
        return bool(str(message.get('type', '')).startswith('系统') and root is not None
                    and root.tag == 'sysmsg' and root.get('type') == 'revokemsg' and root.find('revokemsg') is not None)

    def mentions(self, group, op):
        if op['kind'] != 'text' or not re.search(r'[@＠]', op['text']):
            return op
        # 已解析的成员不能在重试时再按可能重名的显示名解析。
        if op.get('atnames') is not None:
            return op
        explicit = r'@微信[（(]([^()（）\r\n]{1,128})[）)]'
        if re.search(r'@微信[（(]', re.sub(explicit, '', op['text'])):
            raise Unavailable('微信 @ 格式不完整，请使用 @昵称 或 @微信(昵称或备注)；消息已保留')
        members = self.db.get_group_members(group)
        aliases = {name for member in members for name in (member.get('nick_name'), member.get('remark'))
                   if isinstance(name, str) and name and not re.search(r'[\r\n]', name)}
        pattern = explicit
        if aliases:
            pattern += (r'|(?<![A-Za-z0-9_@./\\+-])[@＠]('
                        + '|'.join(re.escape(name) for name in sorted(aliases, key=lambda name: (-len(name), name)))
                        + r')(?![\w@/\\-]|[.]\w)')
        recipients = []
        names = {}
        def replace(match):
            name = match[1].strip() if match[1] is not None else match[2]
            found = {m['username']: m for m in members if name and name in (m.get('nick_name'), m.get('remark'))}
            if len(found) != 1:
                raise Unavailable(f'微信 @ 成员“{name}”未找到或重名，请使用唯一昵称或备注；消息已保留')
            member = next(iter(found.values()))
            recipients.append(member['username'])
            token = '@' + (member.get('nick_name') or name) + '\u2005'
            names[member['username']] = token
            return token
        text = re.sub(pattern, replace, op['text'])
        if not recipients:
            return op
        return dict(op, text=text, atlist=','.join(dict.fromkeys(recipients)), atnames=names)

    def groups(self):
        conn = self.db._contact_conn()
        try:
            rows = conn.execute("SELECT username,nick_name,remark FROM contact WHERE username LIKE '%@chatroom' "
                                "AND is_in_chat_room=1 AND delete_flag=0").fetchall()
        finally:
            conn.close()
        return [{"id": row["username"], "name": row["remark"] or row["nick_name"] or row["username"],
                 "group_name": row["nick_name"] or "", "remark": row["remark"] or ""} for row in rows]

    def notice(self, group):
        conn = self.db._contact_conn()
        try:
            row = conn.execute("SELECT announcement_,announcement_editor_,announcement_publish_time_ "
                               "FROM chat_room_info_detail WHERE username_=?", (group,)).fetchone()
            if not row or not row[0]:
                return None
            content = row[0].decode("utf-8") if isinstance(row[0], bytes) else str(row[0])
            return {"content": content, "sender": self.db.get_nickname(row[1]) if row[1] else "群公告",
                    "version": str(row[2]) + ":" + hashlib.sha256(content.encode()).hexdigest()}
        finally:
            conn.close()

    def _set_image_probe(self, path):
        with open(path, 'rb') as cached:
            is_v2 = cached.read(6) == b'\x07\x08V2\x08\x07'
        if is_v2:
            # 旧格式缓存不能验证当前图片的 AES 密钥。
            self.media._key_probe = None
            self.media._probe_ct(str(path))

    def media_path(self, group, msg, kind):
        if kind == 'file':
            root = xml_root(msg.get('content', ''))
            app = root.find('.//appmsg') if root is not None else None
            if app is None or text_at(app, 'type') not in ('6', '74'):
                raise Unavailable('微信消息没有有效的文件附件信息')
            # 微信 4.x 的文件类型含子类型高位，SDK 按 local_type=49 查不到这些文件。
            return self.file_path(text_at(app, 'title'), text_at(app, 'totallen'), text_at(app, 'md5', 'filemd5'), wx_key(group, msg))
        method = {"image": "download_image", "video": "download_video", "record": "download_voice"}[kind]
        dest_dir = self.media_dir / hashlib.sha256(wx_key(group, msg).encode()).hexdigest()
        dest_dir.mkdir(exist_ok=True)
        args = {"shard": msg.get("shard") or None, 'save_dir': str(dest_dir)}
        if kind == "image":
            args["tier"] = "full"
            row = self.db.get_message_row(group, int(msg['local_id']), local_type=3, shard=args['shard'])
            if row is not None:
                files = self.media._image_files(group, self.media._img_md5(row))
                picked = self.media._pick_full(files, (files.get('mid') or (0, 0))[1])
                if picked:
                    self._set_image_probe(picked[0][0])
        try:
            path = getattr(self.media, method)(group, int(msg["local_id"]), **args)
        except struct.error:
            if kind != 'image':
                raise
            raise MediaPending('微信图片缓存还在写入，完整校验后会自动补发') from None
        except (RuntimeError, OSError) as exc:
            if kind == 'video':
                path = None
            elif kind == 'image' and 'AES' in str(exc):
                raise Unavailable('微信图片解密密钥未加载：请在微信点击任意图片查看大图，再重试此消息') from exc
            else:
                raise
        if kind == 'video':
            return self.video_path(group, msg, path)
        if not path or not Path(path).is_file():
            raise MediaPending("微信媒体尚未在本机完整下载，缓存就绪后会自动补发")
        source = Path(path).resolve()
        if source.stat().st_size > MAX_FILE:
            raise Unavailable("单个媒体文件超过 512 MiB")
        if kind == 'image':
            from PIL import Image
            try:
                with Image.open(source) as image:
                    image.verify()
            except (OSError, ValueError) as exc:
                raise Unavailable('微信图片缓存无法完整解码，请在微信重新打开图片后重试') from exc
        # A library output named only by local_id can collide across DB shards.
        dest = dest_dir / source.name
        if source != dest.resolve():
            shutil.copyfile(source, dest)
        return str(dest.resolve())

    def video_path(self, group, msg, cached=None):
        root = xml_root(msg.get('content', ''))
        video = root.find('.//videomsg') if root is not None else None
        size = video.get('length', '') if video is not None else ''
        hashes = {video.get(field, '').lower() for field in ('md5', 'newmd5')} if video is not None else set()
        hashes = {value for value in hashes if re.fullmatch(r'[a-f0-9]{32}', value)}
        if not size.isdigit() or int(size) <= 0 or not hashes:
            raise MediaPending('微信视频元数据尚未就绪，大小和 MD5 完整后会自动补发')
        length = int(size)
        if length > MAX_FILE:
            raise Unavailable('视频超过本程序 512 MiB 附件上限，已保留源消息')
        key = 'video:' + wx_key(group, msg)
        ident = hashlib.sha256(key.encode()).hexdigest()
        directory = self.media_dir / ident
        directory.mkdir(exist_ok=True)
        destination = directory / 'video.mp4'
        native_root = (self.directory.parent / 'feishu/media').resolve()
        partial = native_root / 'wechat-video' / (ident + '.mp4.part')

        def complete(path):
            try:
                if not path.is_file() or path.stat().st_size != length:
                    return False
                with path.open('rb') as stream:
                    return hashlib.file_digest(stream, 'md5').hexdigest() in hashes
            except OSError:
                return False

        if not directory.resolve().is_relative_to(self.media_dir.resolve()):
            raise Unavailable('微信视频缓存目录不在私人媒体目录内')
        candidates = [destination, partial]
        if cached:
            source = Path(cached).resolve()
            if source.is_relative_to(self.media_dir.resolve()):
                candidates.append(source)
        for source in candidates:
            allowed = source.resolve().is_relative_to(self.media_dir.resolve()) or source.resolve().is_relative_to(native_root)
            if not allowed or not complete(source):
                continue
            if source != destination:
                temporary = destination.with_suffix('.mp4.tmp')
                if temporary.is_symlink():
                    raise Unavailable('微信视频临时缓存不能是链接，未写入')
                try:
                    shutil.copyfile(source, temporary)
                    if not complete(temporary):
                        raise MediaPending('微信视频在读取时发生变化，会重新校验后自动补发')
                    temporary.replace(destination)
                except OSError:
                    raise MediaPending('微信视频缓存正在写入，会自动重试') from None
            return str(destination.resolve())
        file_id, aes = video.get('cdnvideourl', ''), video.get('aeskey', '')
        if len(file_id) % 2 or not re.fullmatch(r'[a-fA-F0-9]{32,4096}', file_id) or not re.fullmatch(r'[a-fA-F0-9]{32}', aes):
            raise Unavailable('微信视频缺少有效的后台下载参数，源消息已保留')
        if not self.native:
            raise MediaPending('微信视频后台下载组件未连接，开始同步后会自动补发')
        try:
            result = self.native.download_video(group, file_id, aes, length, key)
        except (OSError, RuntimeError, ValueError):
            raise MediaPending('微信视频后台下载暂未启动，会自动重试；无需操作微信窗口') from None
        if not isinstance(result, dict) or result.get('ret') != 0 or result.get('status') != 'downloading':
            raise MediaPending('微信视频后台下载未确认，会自动重试')
        if not partial.resolve().is_relative_to(native_root) or Path(result.get('path', '')).resolve() != partial.resolve():
            raise Unavailable('微信视频后台下载返回了不匹配的缓存路径，未读取')
        raise MediaPending('微信视频正在后台下载，完整校验后会自动转发')

    def emoji_path(self, content):
        from Cryptodome.Cipher import AES
        from Cryptodome.Util.Padding import unpad
        from PIL import Image
        root = xml_root(content)
        emoji = root.find('.//emoji') if root is not None else None
        digest = (emoji.get('md5', '') if emoji is not None else '').lower()
        if not re.fullmatch(r'[a-f0-9]{32}', digest):
            raise Unavailable('微信表情缺少有效的原资源校验信息')
        size = emoji.get('len', '')
        if size and (not size.isdigit() or int(size) <= 0):
            raise Unavailable('微信表情的原资源大小无效')
        expected = int(size) if size else None
        if expected is not None and expected > MAX_IMAGE:
            raise Unavailable('微信表情超过飞书图片 10 MB 上传上限')
        directory = self.media_dir / 'emoji' / digest
        if not directory.resolve().is_relative_to(self.media_dir.resolve()):
            raise Unavailable('微信表情缓存目录不在私人媒体目录内')
        directory.mkdir(parents=True, exist_ok=True)
        for cached in directory.glob(digest + '.*'):
            if cached.is_file() and cached.suffix != '.tmp' and cached.resolve().is_relative_to(self.media_dir.resolve()):
                length = cached.stat().st_size
                if 0 < length <= MAX_IMAGE and (expected is None or length == expected) and hashlib.md5(cached.read_bytes()).hexdigest() == digest:
                    return str(cached.resolve())
        urls = list(dict.fromkeys(emoji.get(field, '') for field in ('cdnurl', 'encrypturl') if emoji.get(field)))
        if not urls:
            raise Unavailable('微信表情没有可下载的原资源地址，消息已保留')
        for url in urls:
            try:
                data = emoji_resource(url)
                if hashlib.md5(data).hexdigest() != digest:
                    key = emoji.get('aeskey', '')
                    if not re.fullmatch(r'[a-fA-F0-9]{32}', key) or not data or len(data) % 16:
                        continue
                    data = unpad(AES.new(bytes.fromhex(key), AES.MODE_ECB).decrypt(data), 16)
                if hashlib.md5(data).hexdigest() != digest:
                    continue
                if expected is not None and expected != len(data):
                    continue
                with Image.open(io.BytesIO(data)) as image:
                    if image.format not in ('GIF', 'PNG', 'JPEG', 'WEBP', 'BMP'):
                        raise Unavailable('微信表情的原图片格式暂不支持，消息已保留')
                    limit = 2000 if image.format == 'GIF' else 12000
                    if max(image.size) > limit:
                        raise Unavailable('微信表情分辨率超过飞书图片限制，消息已保留')
                    extension = image.format.lower()
                    for frame in range(getattr(image, 'n_frames', 1)):
                        image.seek(frame)
                        image.load()
                destination = directory / (digest + '.' + extension)
                temporary = destination.with_suffix('.tmp')
                temporary.write_bytes(data)
                temporary.replace(destination)
                return str(destination.resolve())
            except (MediaPending, OSError, ValueError):
                continue
        raise MediaPending('微信表情原资源暂未下载或未通过完整性校验，会自动重试')

    def file_path(self, name, size, digest, key):
        digest = str(digest or '').lower().strip()
        if not name or re.search(r'[\\/:*?"<>|\x00-\x1f]', name) or name.rstrip(' .') != name or Path(name).is_reserved():
            raise Unavailable('微信文件没有有效的原文件名，已保留消息')
        if not digest:
            raise MediaPending('微信文件元数据尚未就绪，MD5 完整后会自动补发')
        if not re.fullmatch(r'[0-9a-f]{32}', digest):
            raise Unavailable('微信文件缺少有效校验信息，不能确认本地附件')
        if size not in (None, '') and (not str(size).isdigit() or int(size) <= 0):
            raise Unavailable('微信文件的大小信息无效，已保留消息')
        expected = int(size) if size not in (None, '') else None
        if expected is not None and expected > MAX_FILE:
            raise Unavailable('单个媒体文件超过 512 MiB')
        root = (Path(self.db.account_dir) / 'msg/file').resolve()
        candidates = []
        for path in root.rglob('*'):
            if not path.is_file() or not path.resolve().is_relative_to(root):
                continue
            length = path.stat().st_size
            if 0 < length <= MAX_FILE and (length == expected if expected is not None else path.name == name):
                candidates.append(path)
        candidates.sort(key=lambda path: path.name != name)
        for path in candidates:
            with path.open('rb') as source:
                if hashlib.file_digest(source, 'md5').hexdigest() != digest:
                    continue
            directory = self.media_dir / hashlib.sha256(key.encode()).hexdigest()
            directory.mkdir(exist_ok=True)
            dest = directory / name
            shutil.copyfile(path, dest)
            with dest.open('rb') as copied:
                complete = hashlib.file_digest(copied, 'md5').hexdigest() == digest
            if not complete or (expected is not None and dest.stat().st_size != expected):
                raise Unavailable('微信文件在读取时发生变化，请下载完整后重试')
            return str(dest.resolve())
        raise MediaPending('未找到与微信消息对应的完整文件，在微信下载后会自动补发')

    def record_media(self, group, item):
        if item['kind'] == 'file':
            return self.file_path(item.get('name', ''), item.get('size'), item.get('md5'),
                                  'record_file:' + str(item.get('md5')) + ':' + str(item.get('name')))
        import re
        digest = item.get("md5", "").lower()
        if not re.fullmatch(r"[0-9a-f]{32}", digest) or item["kind"] != "image":
            raise Unavailable("合并转发附件没有可读取的本地资源，不能只转发标题")
        account = Path(self.db.account_dir)
        temporary = (account / 'temp/RWTemp').resolve()
        expected = int(item['size']) if str(item.get('size', '')).isdigit() else None
        for candidate in temporary.rglob('*'):
            if not candidate.is_file() or not candidate.resolve().is_relative_to(temporary) or candidate.suffix.lower() not in (
                    '.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp'):
                continue
            length = candidate.stat().st_size
            if not 0 < length <= MAX_FILE or (expected is not None and length != expected):
                continue
            with candidate.open('rb') as cached:
                if hashlib.file_digest(cached, 'md5').hexdigest() != digest:
                    continue
            from PIL import Image
            try:
                with Image.open(candidate) as image:
                    image.verify()
            except (OSError, ValueError):
                continue
            directory = self.media_dir / hashlib.sha256(('record-image:' + digest).encode()).hexdigest()
            directory.mkdir(exist_ok=True)
            destination = directory / (digest + candidate.suffix.lower())
            shutil.copyfile(candidate, destination)
            with destination.open('rb') as copied:
                if hashlib.file_digest(copied, 'md5').hexdigest() != digest:
                    raise Unavailable('合并转发图片缓存在复制时发生变化，未发送')
            return str(destination.resolve())
        candidates = list((account / "msg" / "attach").rglob(digest + ".dat"))
        candidates += list((account / "msg" / "attach").rglob(digest + "_h.dat"))
        attachment_root = (account / 'msg/attach').resolve()
        record_root = attachment_root / hashlib.md5(group.encode()).hexdigest()
        for folder in record_root.glob('????-??/Rec/*/Img'):
            candidates.extend(path for path in folder.iterdir() if path.is_file() and path.resolve().is_relative_to(attachment_root)
                              and 0 < path.stat().st_size <= MAX_FILE)
        if not candidates:
            raise MediaPending("合并转发图片没有本地缓存，加载记录内的图片后会自动补发")
        for candidate in candidates:
            try:
                if not candidate.resolve().is_relative_to(attachment_root) or not 0 < candidate.stat().st_size <= MAX_FILE:
                    continue
                self._set_image_probe(candidate)
                data = self.media.decrypt_image(str(candidate))
                if not data or hashlib.md5(data).hexdigest() != digest or (expected is not None and len(data) != expected):
                    continue
                path = self.media._write_decrypted(data, digest, str(self.media_dir))
                if path and Path(path).is_file():
                    if not Path(path).resolve().is_relative_to(self.media_dir.resolve()) or hashlib.md5(Path(path).read_bytes()).hexdigest() != digest:
                        continue
                    from PIL import Image
                    with Image.open(path) as image:
                        image.verify()
                    return path
            except Exception:
                continue
        raise Unavailable("合并转发图片缓存无法解密")

    def server_id(self, group, msg):
        if not msg.get('shard'):
            return ''
        row = self.db.get_message_row(group, msg['local_id'], shard=msg['shard'])
        if not row or row['sort_seq'] != msg['sort_seq']:
            return ''
        return str(int(row['server_id']) % (1 << 64)) if row.get('server_id') else ''

    def message_identity(self, group, msg):
        content = str(msg.get("content", ""))
        prefix = re.match(r'^([A-Za-z0-9_-]{1,128}):\r?\n', content) if group.endswith('@chatroom') else None
        if prefix and not prefix[1].startswith('wxid_') and prefix[1] != msg.get('sender_username'):
            known_name = self.db.get_display_name(group, prefix[1]) if hasattr(self.db, 'get_display_name') else prefix[1]
            if known_name == prefix[1]:
                prefix = None
        username = prefix[1] if prefix else msg.get('sender_username') or (self.db.wxid if msg.get('sender_id') == 2 else '')
        if prefix:
            content = content[prefix.end():]
        return content, username

    def to_feishu(self, group, msg, include_quote=True):
        kind = msg.get("type", "")
        if wechat_system_message(msg):
            return []
        content, username = self.message_identity(group, msg)
        resolve_name = getattr(self.db, 'get_display_name', lambda group, user: self.db.get_nickname(user))
        sender = display_name(resolve_name(group, username)) if username else ('我' if msg.get('sender_id') == 2 else '群成员')
        if sender == username:
            sender = '我' if msg.get('sender_id') == 2 else '群成员'
        header = f"{sender or '群成员'}："
        def text_op(body, name=sender):
            op = {'kind': 'text', 'text': (name or '群成员') + '：' + body, 'sender': name or '群成员'}
            if any(keyword in body for keyword in ALERT_NAMES):
                op['mention_all'] = True
            return op
        if kind == "文本":
            return [text_op(content)]
        if kind == '动画表情':
            return [{'kind': 'text', 'text': header, 'sender': sender or '群成员'}, {'kind': 'image', 'path': self.emoji_path(content)}]
        if kind in ("图片", "语音", "视频"):
            media_kind = {"图片": "image", "语音": "record", "视频": "video"}[kind]
            return [{"kind": "text", "text": header, "sender": sender or '群成员'},
                    {"kind": media_kind, "path": self.media_path(group, msg, media_kind)}]
        root = xml_root(content)
        if root is not None:
            app = root.find(".//appmsg")
            app_type = text_at(app, "type") if app is not None else ""
            if app_type == "19":
                items = record_items(root)
                attachments = [item for item in items if item['kind'] != 'text']
                ops = []
                for item in items:
                    if item.get('sender_username'):
                        name = display_name(resolve_name(group, item['sender_username']))
                        if name and name != item['sender_username']:
                            item['sender'] = name
                        elif item['sender'] == item['sender_username']:
                            item['sender'] = '群成员'
                    if item["kind"] == "text":
                        ops.append(text_op(item['text'], item['sender']))
                    else:
                        label = {'image': '图片', 'file': '文件', 'record': '语音', 'video': '视频'}[item['kind']]
                        ops.append({'kind': 'text', 'text': item['sender'] + '：[' + label + ']' + item.get('name', ''), 'sender': item['sender']})
                return ops + [{'kind': 'record_media', 'item': item} for item in attachments]
            if app_type in ("6", "74"):
                return [{"kind": "text", "text": header + text_at(app, 'title'), "sender": sender or '群成员'}, {"kind": "file", "path": self.media_path(group, msg, "file")}]
            if app_type in ("5", "57"):
                pieces = [app.findtext('./title')]
                if app_type == '5':
                    pieces.extend([app.findtext('./des'), app.findtext('./url')])
                op = text_op(" ".join(dict.fromkeys(filter(None, pieces))))
                if app_type == "57" and include_quote:
                    quote = root.find(".//refermsg")
                    if quote is not None:
                        user = text_at(quote, 'fromusr')
                        name = display_name(resolve_name(group, user)) if user else ''
                        pieces.append("引用 " + (name if name and name != user else display_name(text_at(quote, 'displayname'), user) or '群成员') + "：" + quote_text(quote))
                body = ('\n' if app_type == '57' and include_quote else ' ').join(dict.fromkeys(filter(None, pieces)))
                if body:
                    op['text'] = header + body
                    return [op]
            title = text_at(root, "title", "content", "label")
            url = text_at(root, "url")
            if title:
                return [text_op(title + (" " + url if url else ""))]
        raise Unavailable(f"微信消息类型「{kind}」没有完整转换方式，已保留原消息待处理")

    def avatar_path(self, group, msg):
        from io import BytesIO
        from PIL import Image, ImageOps
        if '合并' in str(msg.get('type', '')):
            return None
        try:
            body, actor = self.message_identity(group, msg)
            root = xml_root(body)
            if root is not None and root.findtext('.//appmsg/type') == '19':
                return None
            content = self.db.get_avatar(actor)
            if not content or len(content) > 2 * 1024 * 1024:
                return None
            with Image.open(BytesIO(content)) as image:
                if not 0 < image.width <= 2048 or not 0 < image.height <= 2048:
                    return None
                image.load()
                thumbnail = ImageOps.fit(image.convert('RGB'), (64, 64), method=Image.Resampling.LANCZOS)
            folder = self.media_dir / hashlib.sha256(('avatar:' + actor).encode()).hexdigest()
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / 'avatar.png'
            thumbnail.save(path)
            return str(path)
        except Exception:
            # 头像是可选外观，缺失缓存不能阻止正文或附件同步。
            return None

    def matches_outgoing(self, group, message, expected):
        if message.get('sender_id') != 2 or self.message_identity(group, message)[1] != self.db.wxid:
            return False
        kind = expected['kind']
        if kind == 'text':
            if message.get('type') != '文本' or self.message_identity(group, message)[0] != expected['text']:
                return False
            if expected['atlist']:
                source = message.get('source')
                if not source:
                    row = self.db.get_message_row(group, message['local_id'], shard=message['shard'])
                    source = row.get('source') if row else ''
                root = xml_root(source)
                at = text_at(root, 'atuserlist') if root is not None else ''
                if not set(expected['atlist'].split(',')) <= set(at.split(',')):
                    return False
            return True
        if (kind == 'image' and message.get('type') != '图片' or
            kind == 'gif' and message.get('type') != '动画表情' or
            kind == 'file' and not message.get('type', '').startswith('文件')
            and not (expected.get('video_attachment') and message.get('type') == '视频')):
            return False
        root = xml_root(self.message_identity(group, message)[0])
        if root is None:
            return False
        if kind in ('image', 'gif'):
            resource = root.find('.//emoji' if kind == 'gif' else './/img')
            return (resource is not None and resource.get('md5') == expected['md5'] and
                (kind != 'gif' or resource.get('len') == str(expected['size'])))
        if expected.get('video_attachment') and message.get('type') == '视频':
            video = root.find('.//videomsg')
            return video is not None and video.get('md5') == expected['md5'] and video.get('length') == str(expected['size'])
        if expected.get('gif_attachment') and text_at(root, 'type') == '8':
            return (text_at(root, 'fileext').lower() == 'gif' and text_at(root, 'emoticonmd5') == expected['md5']
                and text_at(root, 'totallen') == str(expected['size']))
        return (text_at(root, 'type') in ('6', '74') and text_at(root, 'title') == expected['name'] and
            text_at(root, 'md5') == expected['md5'] and text_at(root, 'totallen') == str(expected['size']))

    def send(self, group, op, store, key=''):
        native = getattr(self, 'native', None)
        if native is None:
            raise Unavailable('微信后台发送组件未连接，消息已保留；不操作窗口和剪贴板')
        kind = op.get('kind')
        if kind not in ('text', 'image', 'file') or not key:
            raise Unavailable('微信发送类型或任务标识无效，消息已保留')
        op = self.mentions(group, op)
        if kind == 'text':
            text = op.get('text', '')
            if not text or len(text.encode('utf-8')) > 24000 or '\0' in text:
                raise Unavailable('微信文本为空或超过发送容量，消息已保留')
            expected = {'kind': kind, 'text': text, 'atlist': op.get('atlist', '')}
        else:
            path = Path(op.get('path', '')).resolve()
            root = (self.directory.parent / 'feishu/media').resolve()
            if not path.is_relative_to(root) or not path.is_file() or not 0 < path.stat().st_size <= MAX_FILE:
                raise Unavailable('飞书附件不完整或不在私人媒体目录内，消息已保留')
            if kind == 'image':
                from PIL import Image
                try:
                    with Image.open(path) as image:
                        if image.format == 'GIF':
                            # shortcut: 动画通道保守使用 1 MiB；确认更大 GIF 可发送后再放宽。
                            if path.stat().st_size > 1024 * 1024 or max(image.size) > 2000:
                                kind = 'file'
                            else:
                                for frame in range(image.n_frames):
                                    image.seek(frame)
                                    image.load()
                                kind = 'gif'
                        else:
                            image.verify()
                except (OSError, ValueError, struct.error) as exc:
                    raise Unavailable('飞书图片无法解码，消息已保留') from exc
            with path.open('rb') as source:
                header = source.read(12)
                gif_attachment = kind == 'file' and header[:6] in (b'GIF87a', b'GIF89a')
                video_attachment = kind == 'file' and path.suffix.lower() == '.mp4' and header[4:8] == b'ftyp'
                source.seek(0)
                digest = hashlib.file_digest(source, 'md5').hexdigest()
            expected = {'kind': kind, 'md5': digest, 'name': path.name, 'size': path.stat().st_size}
        state_key = 'wx_attempt:' + key
        state = store.state(state_key)
        expected_hash = fingerprint(expected)
        if state and state['expected_hash'] != expected_hash:
            raise Unavailable('重试内容或 @ 对应成员发生变化，请先核对原消息')
        if state and state.get('ack'):
            store.add_echo(state['ack'])
            return state['server_id']
        if state is None or state.get('rejected'):
            try:
                health = native.request('/health')
                if health.get('pid') != native.pid or health.get('error'):
                    raise RuntimeError('微信进程或发送状态发生变化')
                if kind == 'gif' and 'gif' not in health.get('capabilities', []):
                    raise Unavailable('微信后台组件尚不支持 GIF 动画发送，请更新组件')
                before = max((int(m['sort_seq']) for m in self.db.get_messages(group, limit=1)), default=0)
            except Unavailable:
                raise
            except Exception as exc:
                raise Unavailable('微信发送组件或本机数据库未就绪，消息已保留') from exc
            match_expected = dict(expected)
            if kind != 'text':
                match_expected.update(gif_attachment=gif_attachment, video_attachment=video_attachment)
            state = {'expected_hash': expected_hash, 'before': before, 'group': group, 'expected': match_expected}
            store.set_state(state_key, state)
            try:
                response = native.send(group, text, key, expected['atlist']) if kind == 'text' else native.send_media(group, path, kind, key)
            except Exception as exc:
                import urllib.error
                if isinstance(exc, urllib.error.HTTPError) and exc.code in (400, 401, 404, 413):
                    state['rejected'] = True
                    store.set_state(state_key, state)
                    raise Unavailable('微信组件拒绝了发送请求，消息已保留') from exc
                raise Uncertain('微信发送请求未收到确认，请查看目标群后再重试') from exc
            state['response'] = response
            store.set_state(state_key, state)
            if response.get('status') != 'queued' or response.get('ret') != 0:
                raise Uncertain('微信发送结果未确认，请查看目标群，未自动重发')
        timeout = 30 if kind == 'text' else 120
        slice_time = getattr(self, 'confirmation_slice', None)
        if slice_time is not None and 'await_started' not in state:
            state['await_started'] = time.time()
            store.set_state(state_key, state)
        remaining = max(0, timeout - (time.time() - state['await_started'])) if slice_time is not None else timeout
        deadline = time.monotonic() + min(remaining, slice_time if slice_time is not None else remaining)
        while time.monotonic() < deadline:
            matches = []
            for message in self.db.get_new_messages(group, since_seq=state['before'], limit=2000):
                match_expected = state.get('expected') or dict(expected)
                if kind != 'text':
                    match_expected = dict(match_expected, gif_attachment=gif_attachment, video_attachment=video_attachment)
                try:
                    matched = self.matches_outgoing(group, message, match_expected)
                except Unavailable:
                    continue
                if not matched:
                    continue
                server_id = self.server_id(group, message)
                if not server_id or not int(server_id):
                    continue
                matches.append((message, server_id))
            if len(matches) > 1:
                raise Uncertain('微信出现多条相同内容，不能唯一确认发送，请查看目标群')
            if matches:
                message, server_id = matches[0]
                state.update(ack=wx_key(group, message), server_id=server_id)
                store.add_echo(state['ack'])
                store.set_state(state_key, state)
                return server_id
            time.sleep(.5)
        if slice_time is not None and time.time() - state['await_started'] < timeout:
            raise ConfirmationPending('等待微信服务端确认；只检查确认，不重新提交')
        raise Uncertain('微信消息已提交，但服务端确认尚未回读；请查看目标群，未自动重发')


class Bridge:
    def __init__(self, wx, fs, directory, bindings, notices, history, log, emit, cloud_files=False,
                 daily_cards=False):
        self.wx, self.fs, self.directory = wx, fs, Path(directory)
        if not bindings or len({b['wx_group'] for b in bindings}) != len(bindings) or len({b['fs_group'] for b in bindings}) != len(bindings):
            raise Unavailable("每个微信群、飞书群只能勾选一个绑定，避免回复被转入错误的群")
        self.pairs = {hashlib.sha256(f"{wx.db.account}:{b['wx_group']}:{fs.app_id}:{b['fs_group']}".encode()).hexdigest(): dict(b)
                      for b in bindings}
        self.fs_pairs = {b['fs_group']: pair for pair, b in self.pairs.items()}
        self.page_limits = {pair: 1000 for pair in self.pairs}
        self.notices, self.history, self.log, self.emit = notices, history, log, emit
        self.cloud_files = cloud_files
        self.daily_cards = daily_cards
        self.latency = {}
        self.awaiting = {}
        self.stop = threading.Event()
        self.commands = queue.Queue()
        self.conversations = {}

    def receive_fs(self, message):
        pair = self.fs_pairs.get(message.get("chat_id"))
        if pair is None or message.get("deleted") or message.get('msg_type') == 'system':
            return
        ident = message.get("message_id")
        if not isinstance(ident, str) or not ident.startswith("om_"):
            raise Unavailable("飞书事件缺少消息 ID，未推进同步进度")
        key = "fs:" + ident
        if self.store.is_echo(key):
            return
        sender = message.get("sender") or {}
        if sender.get("sender_type") == "app" and sender.get("id") == self.fs.app_id:
            return
        if int(message.get("create_time", 0)) < self.store.state(pair + ":fs_floor", 0):
            return
        # 飞书原消息会以微信实际送达的文本加入小时卡片，届时按微信顺序分段。
        if not self.store.state(pair + ':hour_card'):
            self.conversations.pop(pair, None)
        self.store.enqueue(pair, key, "fs_to_wx", message)

    def catch_up(self):
        for pair, binding in self.pairs.items():
            since = self.store.state(pair + ":fs_time", self.store.state(pair + ":fs_floor"))
            end = int(time.time()) + 1
            for message in self.fs.history(binding['fs_group'], max(0, since // 1000 - 5), end):
                if self.stop.is_set():
                    return
                if message.get('chat_id') != binding['fs_group']:
                    raise Unavailable("飞书历史消息来自其他群，未推进同步进度")
                self.receive_fs(message)
            # 翻页完全成功后才推进游标；同一秒以及乱序事件由消息 ID 去重。
            self.store.set_state(pair + ":fs_time", (end - 1) * 1000)

    def import_history(self, pair):
        if self.store.state(pair + ':wx_history_done', False):
            return
        group = self.pairs[pair]['wx_group']
        name = self.pairs[pair].get('name', group)
        boundary = self.store.state(pair + ':wx_history_boundary')
        if boundary is None:
            recent = self.wx.db.get_messages(group, limit=1)
            boundary = max((int(m['sort_seq']) for m in recent), default=0)
            self.store.set_state(pair + ':wx_history_boundary', boundary)
        cursor = self.store.state(pair + ':wx_history_cursor', -1)
        limit = 1000
        while not self.stop.is_set():
            messages = self.wx.db.get_new_messages(group, since_seq=cursor - 1, limit=limit)
            previous = cursor
            for message in messages:
                if self.stop.is_set():
                    return
                seq = int(message['sort_seq'])
                if seq <= boundary:
                    # 历史导入不使用旧版的启动回声标记；队列按真实消息 ID 去重。
                    self.store.enqueue(pair, wx_key(group, message), 'wx_to_fs', message)
                    cursor = max(cursor, seq)
            self.store.set_state(pair + ':wx_history_cursor', cursor)
            count = self.store.conn.execute("SELECT count(*) FROM task WHERE pair=? AND direction='wx_to_fs'", (pair,)).fetchone()[0]
            self.emit('status', f'正在读取历史：{name}，已入队 {count} 条（暂停后可继续）')
            self.report_stats()
            if len(messages) < limit or any(int(m['sort_seq']) > boundary for m in messages):
                self.store.set_state(pair + ':wx_cursor', max(self.store.state(pair + ':wx_cursor', 0), boundary))
                self.store.set_state(pair + ':wx_history_done', True)
                self.log(f'{name}：历史已入队 {count} 条，文字、图片和附件摘要按小时显示为聊天卡片；缺失附件会列入失败列表')
                return
            # 重新包含游标所在的全部消息，避免同一时间戳跨页时漏掉记录。
            limit = limit * 2 if cursor == previous else 1000

    def poll_wechat(self, pair):
        group = self.pairs[pair]['wx_group']
        cursor = self.store.state(pair + ':wx_cursor', 0)
        limit = self.page_limits[pair]
        messages = self.wx.db.get_new_messages(group, since_seq=max(-1, cursor - 1), limit=limit)
        previous = cursor
        deferred = []
        for message in messages:
            key = wx_key(group, message)
            if self.store.native_echo(pair, key):
                self.mirror_message(pair, key)
            elif self.pending_native_message(pair, group, message):
                # A submitted attachment can be visible locally before its server ID arrives.
                # Keep its cursor readable until the sender confirms it and creates the mirror.
                deferred.append(int(message['sort_seq']))
                continue
            elif not self.store.is_echo(key):
                self.store.enqueue(pair, key, 'wx_to_fs', message)
            elif int(message['sort_seq']) > previous:
                self.break_conversation(pair)
            cursor = max(cursor, int(message['sort_seq']))
        if deferred:
            cursor = min(cursor, min(deferred))
        self.store.set_state(pair + ':wx_cursor', cursor)
        self.page_limits[pair] = limit * 2 if len(messages) == limit and cursor == previous else 1000

    def pending_native_message(self, pair, group, message):
        identity = getattr(self.wx, 'message_identity', None)
        if not identity or identity(group, message)[1] != getattr(self.wx.db, 'wxid', ''):
            return False
        rows = self.store.conn.execute("SELECT key,ops,step FROM task WHERE pair=? AND direction IN ('fs_to_wx','card_to_wx') AND status IN ('pending','sending') AND ops IS NOT NULL", (pair,)).fetchall()
        for row in rows:
            operations = json.loads(row['ops'])
            if row['step'] >= len(operations):
                continue
            op = operations[row['step']]
            prefix = 'wx_attempt:' + pair + ':' + row['key'] + ':' + str(row['step']) + ':'
            attempts = [json.loads(r[0]) for r in self.store.conn.execute('SELECT value FROM state WHERE key LIKE ?', (prefix + '%',)).fetchall()]
            attempts = [state for state in attempts if not state.get('ack') and not state.get('rejected') and int(message['sort_seq']) > state.get('before', int(message['sort_seq']))]
            if not attempts:
                continue
            if op['kind'] == 'text' and message.get('type') == '文本' and message.get('content') == op['text']:
                return True
            if op['kind'] not in ('image', 'file'):
                continue
            path = Path(op['path'])
            if not path.is_file():
                continue
            try:
                root = xml_root(message.get('content', ''))
                kind = message.get('type', '')
                node = root.find('.//img' if kind == '图片' else './/emoji' if kind == '动画表情' else './/videomsg' if kind == '视频' else './/appmsg') if root is not None else None
                if node is None:
                    continue
                digest = node.get('md5', '') if kind in ('图片', '动画表情', '视频') else text_at(node, 'emoticonmd5' if text_at(node, 'type') == '8' else 'md5')
                expected = {'kind': 'image' if kind == '图片' else 'gif' if kind == '动画表情' else 'file',
                            'md5': digest.lower(), 'size': path.stat().st_size, 'name': path.name}
                if any(state['expected_hash'] == fingerprint(expected) for state in attempts):
                    return True
            except (Unavailable, OSError, ValueError, KeyError):
                continue
        return False

    def mirror_message(self, pair, ack, floor=0):
        group = self.pairs[pair]['wx_group']
        prefix = 'wx:' + group + ':'
        if not isinstance(ack, str) or not ack.startswith(prefix) or not self.store.native_echo(pair, ack):
            return False
        shard, local_id, sequence = ack[len(prefix):].rsplit(':', 2)
        locator = {'shard': shard, 'local_id': int(local_id), 'sort_seq': int(sequence)}
        message = self.wx.db.reload_message(group, locator)
        kind = message.get('type', '')
        if (kind not in ('文本', '图片', '动画表情', '视频') and not kind.startswith('文件')
                or int(message.get('create_time', 0)) < floor
                or self.wx.message_identity(group, message)[1] != self.wx.db.wxid or wx_key(group, message) != ack):
            return False
        # 已独立转发的附件保留原件；补展示使用不同 ID，不能重发微信或替换原发送计划。
        key = ack if kind == '文本' else 'wx_card:' + ack
        return self.store.enqueue(pair, key, 'wx_to_fs', message)

    def backfill_replies(self, pair):
        # 补回本地已确认、过去一天因防循环跳过的回复；只保存定位，不重新发送微信。
        floor = int(time.time()) - 86400
        rows = self.store.conn.execute("SELECT value FROM state WHERE key LIKE ? AND json_extract(value,'$.ack') IS NOT NULL",
                                       ('wx_attempt:' + pair + ':%',)).fetchall()
        count = 0
        for row in rows:
            if self.stop.is_set():
                break
            try:
                count += bool(self.mirror_message(pair, json.loads(row[0])['ack'], floor))
            except (RuntimeError, ValueError, KeyError, Unavailable):
                continue
        if count:
            self.log(self.pairs[pair].get('name', '绑定群') + f'：补入小时卡片的已送达飞书回复 {count} 条')

    def recall_menu(self, pair, message, echo):
        if not echo or not getattr(self.fs, 'recall_entry_available', False):
            return None

        group = self.pairs[pair]['wx_group']
        if self.wx.message_identity(group, message)[1] != self.wx.db.wxid:
            return None
        ack = wx_key(group, message)
        try:
            origin, _ = self.store.native_origin(pair, ack)
            owner_key = 'fs_owner:' + pair + ':' + origin
            owner = self.store.state(owner_key)
            if owner is None:
                original = next((m for m in self.fs.get_message(origin) if m.get('message_id') == origin), None)
                sender = original.get('sender', {}) if original else {}
                if (not original or original.get('chat_id') != self.pairs[pair]['fs_group'] or original.get('deleted')
                        or sender.get('sender_type') != 'user' or not isinstance(sender.get('id'), str)
                        or not re.fullmatch(r'ou_[A-Za-z0-9_-]{1,128}', sender['id'])):
                    return None
                owner = {'owner': sender['id']}
                self.store.set_state(owner_key, owner)
            source = dict({name: message[name] for name in ('local_id', 'sort_seq', 'shard')}, pair=pair,
                chat=self.pairs[pair]['fs_group'], owner=owner['owner'], origin=origin, ack=ack, time=int(message['create_time']))
            token = hashlib.sha256((pair + ':' + ack).encode()).hexdigest()
            previous = self.store.state('recall_source:' + token)
            if previous is not None and previous != source:
                return None
            self.store.set_state('recall_source:' + token, source)
            return {'target': 'rc_' + token, 'label': '撤回 ' + time.strftime('%H:%M:%S', time.localtime(source['time']))}
        except (Unavailable, ValueError, KeyError):
            # 入口来源无法确认时，仍正常转发正文和附件。
            return None

    def refresh_recalled_card(self, source):
        pair = source['pair']
        row = self.store.conn.execute('SELECT id FROM receipt WHERE pair=? AND key IN (?,?) LIMIT 1',
            (pair, source['ack'], 'wx_card:' + source['ack'])).fetchone()
        if not row:
            raise Unavailable('撤回已确认，但卡片来源记录暂不可读取')
        contexts = [json.loads(row[0]) for row in self.store.conn.execute('SELECT value FROM state WHERE key LIKE ? OR key LIKE ?',
            (pair + ':hour_card%', pair + ':day_card%')).fetchall()]
        targets = {context['message_id']: context for context in contexts if any(
            all(locator.get(name) == source[name] for name in ('local_id', 'sort_seq', 'shard'))
            for locator in context.get('sources', []))}
        if not targets:
            raise Unavailable('撤回已确认，但原卡片定位暂不可读取')
        try:
            for card, context in targets.items():
                if context.get('period') == 'day':
                    index = next(i for i, locator in enumerate(context['sources']) if all(
                        locator.get(name) == source[name] for name in ('local_id', 'sort_seq', 'shard')))
                    context['page'] = bisect.bisect_right(context['pages'], index) + 1
                rendered = self.render_hour_context(pair, context, row['id'])
                rendered['append_to'] = card
                self.fs.update_hour_card(self.pairs[pair]['fs_group'], rendered, 'recall-card:' + source['target'] + ':' + card)
        finally:
            self.store.cleanup_completed_files()

    def recall_action(self, event):
        from native_recall import TEST_GROUP
        from recall import recall_feedback

        def feedback(text, kind='warning'):
            return {'toast': {'type': kind, 'content': text}}

        def submit(source):
            pair = source['pair']
            binding = self.pairs.get(pair)
            caller = getattr(self.wx, 'recaller', None)
            if not binding or binding['wx_group'] != TEST_GROUP:
                return feedback('当前仅在测试群启用微信撤回，其他群的消息保持不变。')
            if caller is None:
                return feedback('测试群微信撤回组件未连接，原消息已保留。')
            key = 'recall_attempt:' + source['target']
            previous = self.store.state(key)
            group = binding['wx_group']
            if previous and previous.get('phase') == 'confirmed':
                try:
                    with self.wx.db.snapshot_scope():
                        self.refresh_recalled_card(source)
                except (Unavailable, Uncertain):
                    return feedback('微信已确认撤回；卡片更新暂未确认，飞书原消息保留。')
                return feedback('微信已确认撤回，卡片已显示撤回提示；飞书原消息保留。', 'success')
            if previous:
                return feedback('这条撤回请求已有处理记录，请先核对微信；不会重复提交。')
            with self.wx.db.snapshot_scope():
                message = self.wx.db.reload_message(group, source)
                if (self.wx.server_id(group, message) != source['server_id'] or int(message['create_time']) != source['time']
                        or self.wx.message_identity(group, message)[1] != self.wx.db.wxid
                        or str(message.get('type', '')).startswith('系统')):
                    return feedback('微信原消息或发送账号无法核对，未提交撤回。')
            # Receipt is stored before the native call; uncertain requests are never resubmitted.
            self.store.set_state(key, {'server_id': source['server_id'], 'phase': 'started', 'time': int(time.time())})
            try:
                result = caller.recall(group, source)
            except urllib.error.HTTPError:
                self.store.set_state(key, {'server_id': source['server_id'], 'phase': 'rejected', 'time': int(time.time())})
                return feedback('微信撤回未获接受：完整原生对象缺失、定位不符或已超时；原消息保留。')
            if result.get('status') != 'queued' or result.get('server_id') != source['server_id']:
                return feedback('撤回提交结果暂未确认，请核对微信；未重复发送。')
            deadline = time.monotonic() + 20
            while not self.stop.is_set() and time.monotonic() < deadline:
                progress = caller.result(source['server_id'])
                if progress.get('processed') and not progress.get('success'):
                    self.store.set_state(key, {'server_id': source['server_id'], 'phase': 'rejected', 'time': int(time.time())})
                    return feedback('微信后台拒绝了撤回，原消息保留。')
                with self.wx.db.snapshot_scope():
                    confirmed = (progress.get('executed') and progress.get('processed') and progress.get('success')
                                 and self.wx.recalled(group, source))
                if confirmed:
                    self.store.set_state(key, {'server_id': source['server_id'], 'phase': 'confirmed', 'time': int(time.time())})
                    try:
                        with self.wx.db.snapshot_scope():
                            self.refresh_recalled_card(source)
                    except (Unavailable, Uncertain):
                        return feedback('微信已确认撤回；卡片更新暂未确认，飞书原消息保留。')
                    return feedback('微信已确认撤回，卡片已显示撤回提示；飞书原消息保留。', 'success')
                self.stop.wait(.3)
            return feedback('微信已接收撤回任务，但实际撤回暂未确认；请核对微信，未重复提交。')

        try:
            response = recall_feedback(self.directory, set(self.fs_pairs), event, submit=submit)
        except sqlite3.Error:
            self.store.conn.rollback()
            response = feedback('撤回状态暂时无法保存，原消息保留；普通同步继续运行，请核对微信后再操作。')
        except (RuntimeError, OSError, ValueError, KeyError):
            response = feedback('撤回结果暂时无法确认，请核对微信；不会自动重试或删除飞书原消息。')
        if response is not None:
            self.log('撤回入口：' + response['toast']['content'])
        return response

    def echo_media_hash(self, group, message):
        body = self.wx.message_identity(group, message)[0]
        root = xml_root(body)
        kind = message['type']
        path = './/img' if kind == '图片' else './/emoji' if kind == '动画表情' else './/videomsg' if kind == '视频' else './/appmsg'
        node = root.find(path) if root is not None else None
        if node is None:
            raise Unavailable('已送达附件的微信原资源信息缺失，未修改卡片')
        if kind in ('图片', '动画表情', '视频'):
            digest = node.get('md5', '').lower()
            size = node.get({'图片': 'hdlength', '动画表情': 'len', '视频': 'length'}[kind], '0') or '0'
        else:
            digest = text_at(node, 'emoticonmd5' if text_at(node, 'type') == '8' else 'md5').lower()
            size = text_at(node, 'totallen')
        return fingerprint({'type': kind, 'md5': digest, 'size': size, 'server': self.wx.server_id(group, message)})

    def echo_asset_ops(self, group, message, asset):
        if asset['wx_hash'] != self.echo_media_hash(group, message):
            raise Unavailable('已归档附件与微信原消息不一致，未修改卡片')
        header = post_ops(self.wx.to_feishu(group, dict(message, type='文本', content='')))[0]['paragraphs']
        if asset['kind'] == 'image':
            node = {'tag': 'img', 'img_key': asset['img_key']}
        else:
            node = {'tag': 'attachment', 'name': asset['name'], 'size': asset['size'],
                    'media_type': asset['kind'], 'download_url': asset['url']}
            if asset.get('cover_key'):
                node['cover_key'] = asset['cover_key']
        return [{'kind': 'post', 'paragraphs': header + [[node]]}]

    def wechat_ops(self, pair, message, echo=False, task_id=None, summary_only=False, attachment_paths=None):
        group = self.pairs[pair]['wx_group']
        if not echo or message.get('type') == '文本':
            ops = post_ops(self.wx.to_feishu(group, message))
            if attachment_paths is not None:
                attachment_paths.extend(op['path'] for op in ops if op['kind'] in ('file', 'video'))
            lines = hour_paragraphs(ops) if summary_only else None
            return [{'kind': 'post', 'paragraphs': lines}] if lines is not None else ops
        from feishu import feishu_to_wx
        ident, index = self.store.native_origin(pair, wx_key(group, message))
        asset_key = 'echo_asset:' + pair + ':' + hashlib.sha256(wx_key(group, message).encode()).hexdigest()
        cached_asset = self.store.state(asset_key)
        if cached_asset:
            return self.echo_asset_ops(group, message, cached_asset)
        items = self.fs.get_message(ident)
        original = next((item for item in items if item.get('message_id') == ident), None)
        if not original or original.get('chat_id') != self.pairs[pair]['fs_group'] or original.get('deleted'):
            raise Unavailable('已送达附件的飞书原消息不在绑定群或已撤回，未修改卡片')
        converted = split_text_ops([self.wx.mentions(group, op) for op in feishu_to_wx(self.fs, original, items)])
        if task_id is not None:
            self.store.register_files(task_id, converted)
        if index >= len(converted) or converted[index]['kind'] not in ('image', 'file'):
            raise Unavailable('已送达附件的原发送位置发生变化，未修改卡片')
        path = Path(converted[index]['path'])
        digest, size = hashlib.md5(path.read_bytes()).hexdigest(), path.stat().st_size
        content, _ = self.wx.message_identity(group, message)
        root = xml_root(content)
        node = root.find('.//img' if message['type'] == '图片' else
                         './/emoji' if message['type'] == '动画表情' else
                         './/videomsg' if message['type'] == '视频' else './/appmsg') if root is not None else None
        if node is None:
            raise Unavailable('已送达附件的微信原资源信息缺失，未修改卡片')
        if message['type'] in ('图片', '动画表情', '视频'):
            matches = node.get('md5', '').lower() == digest
            # 图片 length 是压缩图大小；md5 对应的完整原件使用 hdlength。
            size_field = {'图片': 'hdlength', '动画表情': 'len', '视频': 'length'}[message['type']]
            expected_size = node.get(size_field, '')
            if expected_size not in ('', '0'):
                matches = matches and expected_size == str(size)
        else:
            app_type = text_at(node, 'type')
            matches = app_type in ('6', '74', '8') and text_at(node, 'emoticonmd5' if app_type == '8' else 'md5').lower() == digest
            matches = matches and text_at(node, 'totallen') == str(size)
        if not matches or not int(self.wx.server_id(group, message) or 0):
            raise Unavailable('飞书附件与微信已送达原件的大小或校验值不一致，未修改卡片')
        if self.cloud_files:
            asset = {'kind': 'image' if message['type'] in ('图片', '动画表情') else
                'video' if message['type'] == '视频' else 'file', 'md5': digest, 'size': size,
                'name': path.name, 'wx_hash': self.echo_media_hash(group, message)}
            if asset['kind'] == 'image':
                asset['img_key'] = self.fs.image_key(path)
            else:
                uploaded = self.fs.drive_file(self.pairs[pair]['fs_group'], self.pairs[pair].get('name', '微信群'),
                                              path, path.name, self.store, pair)
                asset['url'] = uploaded['url']
                if asset['kind'] == 'video':
                    original_body = json.loads((original.get('body') or {}).get('content', '{}'))
                    if original_body.get('image_key'):
                        cover = self.fs.resource(ident, original_body['image_key'], 'image')
                        if task_id is not None:
                            self.store.register_files(task_id, {'path': cover})
                        asset['cover_key'] = self.fs.image_key(cover)
            self.store.set_state(asset_key, asset)
            return self.echo_asset_ops(group, message, asset)
        header = post_ops(self.wx.to_feishu(group, dict(message, type='文本', content='')))[0]['paragraphs']
        if message['type'] in ('图片', '动画表情'):
            return [{'kind': 'post', 'paragraphs': header + [[{'tag': 'img', 'path': str(path)}]]}]
        attachment = {'tag': 'attachment', 'name': path.name, 'size': size,
                      'media_type': 'video' if message['type'] == '视频' else 'file'}
        if attachment_paths is not None:
            attachment_paths.append(str(path))
        if message['type'] == '视频':
            body = (original.get('body') or {}).get('content', '{}')
            body = json.loads(body) if isinstance(body, str) else body
            if body.get('image_key'):
                cover = self.fs.resource(ident, body['image_key'], 'image')
                if task_id is not None:
                    self.store.register_files(task_id, {'path': cover})
                header = header + [[{'tag': 'img', 'path': cover}]]
        # 卡片 JSON 2.0 没有文件/视频播放器；原件仍在飞书原消息里，不伪造下载地址。
        return [{'kind': 'post', 'paragraphs': header + [[attachment]]}]

    def report_stats(self):
        total, rows, groups = {}, [], {}
        for pair, binding in self.pairs.items():
            counts = self.store.counts(pair)
            groups[binding['wx_group']] = counts
            for status, count in counts.items():
                total[status] = total.get(status, 0) + count
            rows.extend(dict(row, group=binding.get('name', binding['wx_group'])) for row in self.store.failed(pair))
        self.emit('stats', (total, rows, groups))

    def prepare_ops(self, task):
        from feishu import feishu_to_wx
        binding = self.pairs[task['pair']]
        payload = json.loads(task['payload'])
        if task['direction'] == 'card_to_wx':
            raise Unavailable('卡片输入已停用，请在飞书普通聊天框发送消息')
        if payload.get('_source_only') and task['direction'] == 'wx_to_fs':
            try:
                payload = self.wx.db.reload_message(binding['wx_group'], payload)
            except Exception as exc:
                raise Unavailable('微信原消息暂时无法回读，恢复后可重试；未重复发送已确认部分') from exc
            if wx_key(binding['wx_group'], payload) != task['key'].removeprefix('wx_card:'):
                raise Unavailable('微信原消息定位不一致，未发送')
            self.store.update(task['id'], payload=json.dumps(payload, ensure_ascii=False))
        elif payload.get('_source_only') and task['direction'] == 'notice_to_fs':
            notice = self.wx.notice(binding['wx_group'])
            if not notice or 'wx_announcement:' + notice['version'] != task['key']:
                self.store.update(task['id'], status='ignored', error='', payload='{}', ops=None)
                return []
            payload = {'content': notice['content']}
            self.store.update(task['id'], payload=json.dumps(payload, ensure_ascii=False))
        if task['direction'] == 'wx_to_fs' and wechat_system_message(payload):
            self.store.update(task['id'], status='ignored', error='', payload='{}', ops=None)
            return []
        if task['direction'] == 'notice_to_fs':
            return [{'kind': 'announcement', 'text': payload.get('content', payload.get('text', '').removeprefix('群公告：'))}]
        if task['direction'] == 'wx_to_fs':
            source_key = f"wx_source:{task['id']}"
            if self.store.state(source_key) is None:
                source = getattr(self.wx, 'server_id', lambda *args: '')(binding['wx_group'], payload)
                self.store.set_state(source_key, source)
            reference = wechat_reply_id(payload)
            target = self.store.state(task['pair'] + ':wx_sent:' + reference) if reference else None
            if target and not task['ops']:
                ops = post_ops(self.wx.to_feishu(binding['wx_group'], payload, include_quote=False))
                if not ops or ops[0]['kind'] != 'post':
                    raise Unavailable('引用回复没有可发送的正文，已保留')
                ops[0]['reply_to'] = target
                self.store.update(task['id'], ops=json.dumps(ops, ensure_ascii=False))
                return ops
        if task['ops']:
            ops = json.loads(task['ops'])
            if task['direction'] == 'fs_to_wx':
                normalize = getattr(self.wx, 'mentions', lambda group, op: op)
                ops = [normalize(binding['wx_group'], op) for op in ops]
            if all(Path(path).is_file() for path in media_paths(ops[task['step']:])):
                return ops
        if task['direction'] == 'wx_batch_to_fs':
            children = self.store.conn.execute('SELECT * FROM task WHERE batch=? ORDER BY id', (task['id'],)).fetchall()
            lines = []
            for child in children:
                for op in self.prepare_ops(child):
                    if op['kind'] != 'post':
                        raise Unavailable('旧历史批次无法完整重建，请核对飞书后再处理')
                    lines.extend(op['paragraphs'])
            if not lines or post_size(lines) > MAX_POST:
                raise Unavailable('旧历史批次无法完整重建，请核对飞书后再处理')
            ops = [{'kind': 'post', 'paragraphs': lines}]
        elif task['direction'] == 'wx_to_fs':
            ops = self.wechat_ops(task['pair'], payload, self.store.native_echo(task['pair'], task['key']),
                                  task['id'], task['key'].startswith('wx_card:'))
        else:
            items = self.fs.get_message(payload['message_id'])
            message = next((m for m in items if m.get('message_id') == payload['message_id']), None)
            if not message or message.get('chat_id') != binding['fs_group']:
                raise Unavailable('无法确认飞书消息来自所选群，已停止发送')
            sender = message.get('sender') or {}
            if self.store.is_echo('fs:' + payload['message_id']) or (
                    sender.get('sender_type') == 'app' and sender.get('id') == self.fs.app_id):
                self.store.complete(task['id'])
                return []
            if message.get('deleted'):
                raise Unavailable('飞书原消息已撤回')
            if (sender.get('sender_type') == 'user' and isinstance(sender.get('id'), str)
                    and re.fullmatch(r'ou_[A-Za-z0-9_-]{1,128}', sender['id'])):
                self.store.set_state('fs_owner:' + task['pair'] + ':' + payload['message_id'], {'owner': sender['id']})
            normalize = getattr(self.wx, 'mentions', lambda group, op: op)
            ops = split_text_ops([normalize(binding['wx_group'], op) for op in feishu_to_wx(self.fs, message, items)])
        if not ops:
            raise Unavailable('消息没有可发送的正文或附件')
        self.store.update(task['id'], ops=json.dumps(ops, ensure_ascii=False))
        return ops







    def card_task_id(self, pair, context):
        locator = context['sources'][context['pages'][context['page'] - 2] if context['page'] > 1 else 0]
        ack = wx_key(self.pairs[pair]['wx_group'], locator)
        row = self.store.conn.execute('SELECT id FROM task WHERE pair=? AND key IN (?,?) LIMIT 1',
                                     (pair, ack, 'wx_card:' + ack)).fetchone()
        if not row:
            raise Unavailable('卡片原消息的临时资源记录不可读取')
        return row['id']

    def render_hour_context(self, pair, current, ident):
        group = self.pairs[pair]['wx_group']
        identity = self.wx.message_identity
        messages = []
        sources = current['sources']
        if current.get('period') == 'day':
            ends, page = current['pages'], current.get('page', len(current['pages']))
            if not 1 <= page <= len(ends):
                raise Unavailable('日卡片页码无效，未修改卡片')
            sources = sources[ends[page - 2] if page > 1 else 0:ends[page - 1]]
        for source in sources:
            message = self.wx.db.reload_message(group, source)
            body, username = identity(group, message)
            if str(message.get('type', '')).startswith('系统') and int(message.get('create_time', 0)) == source['time']:
                root = xml_root(body)
                # 微信撤回会原地替换旧记录，不能因此阻止后面的消息追加。
                if root is not None and root.tag == 'sysmsg' and root.get('type') == 'revokemsg' and root.find('revokemsg') is not None:
                    token = hashlib.sha256((pair + ':' + wx_key(group, message)).encode()).hexdigest()
                    if self.store.state('recall_attempt:' + token, {}).get('phase') == 'confirmed':
                        messages.append({'time': source['time'], 'self': True, 'recalled': True})
                    continue
            paths = []
            converted = self.wechat_ops(pair, message, source.get('echo', False), ident, source.get('summary_only', False), paths)
            self.store.register_files(ident, converted)
            paragraphs = hour_paragraphs(converted)
            if (message.get('type') != source['type'] or int(message.get('create_time', 0)) != source['time']
                    or hashlib.sha256(username.encode()).hexdigest() != source['actor']
                    or paragraphs is None or not card_source_matches(group, message, converted, source['hash'])):
                raise Unavailable('小时卡片的原消息或发送者发生变化，未修改卡片')
            if self.cloud_files or self.store.state(f'cloud_media:{ident}'):
                paragraphs = [[dict(node) for node in line] for line in paragraphs]
                for node in (node for line in paragraphs for node in line if node['tag'] == 'attachment'):
                    if node.get('download_url'):
                        continue
                    if not paths:
                        raise Unavailable('小时卡片的云盘附件来源缺失，未修改卡片')
                    path = paths.pop(0)
                    asset = self.fs.drive_file(self.pairs[pair]['fs_group'], self.pairs[pair].get('name', '微信群'),
                                               path, node['name'], self.store, pair)
                    node['download_url'] = asset['url']
                    if node.get('media_type') == 'video':
                        import imageio_ffmpeg
                        folder = self.directory / 'feishu/media/drive_covers'
                        folder.mkdir(parents=True, exist_ok=True)
                        cover = folder / (asset['sha256'] + '.jpg')
                        self.store.register_files(ident, {'path': str(cover)})
                        try:
                            result = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-i', path,
                                '-frames:v', '1', '-vf', 'scale=320:-2', '-y', str(cover)], capture_output=True,
                                timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                            if result.returncode == 0 and cover.is_file():
                                node['cover'] = str(cover)
                        except (OSError, subprocess.TimeoutExpired):
                            pass
            avatar = getattr(self.wx, 'avatar_path', lambda *args: None)(group, message)
            if avatar:
                self.store.register_files(ident, {'path': avatar})
            messages.append({'actor': source['actor'], 'time': source['time'], 'turn': source['turn'], 'echo': source.get('echo', False),
                             'self': username == getattr(self.wx.db, 'wxid', ''), 'avatar': avatar,
                             'paragraphs': paragraphs,
                             **({'recall': menu} if (menu := self.recall_menu(pair, message, source.get('echo', False))) else {})})
        card_mentions = {}
        if current.get('period') == 'day' and any(re.search(r'[@＠](?:温大龙|温小虎|温家俊)', node.get('text', ''))
                for message in messages for line in message.get('paragraphs', []) for node in line):
            try:
                card_mentions = getattr(self.fs, 'card_mentions', lambda chat: {})(self.pairs[pair]['fs_group'])
            except (Unavailable, Uncertain):
                self.log('卡片提及成员暂不可读取，保留正文，已有成员通知继续保留')
        return {'kind': 'hour_card', 'hour': current['hour'], 'part': current['part'], 'messages': messages,
                **({'card_mentions': card_mentions} if card_mentions else {}),
                **({'period': 'day', 'page': current['page'], 'page_count': len(current['pages'])} if current.get('period') == 'day' else {})}

    def conversation_ops(self, task, ops):
        from feishu import card_fits, hour_card_content
        pair, ident = task['pair'], task['id']
        saved = self.store.state(f'card_merge:{ident}')
        if (self.daily_cards and not saved) or (saved and saved.get('period') == 'day'):
            return self.daily_conversation_ops(task, ops)
        if saved and 'hour' not in saved:
            return self.legacy_conversation_ops(task, ops)
        payload = json.loads(self.store.conn.execute('SELECT payload FROM task WHERE id=?', (ident,)).fetchone()[0])
        identity = getattr(self.wx, 'message_identity', None)
        lines = hour_paragraphs(ops) if task['direction'] == 'wx_to_fs' else None
        eligible = (task['direction'] == 'wx_to_fs' and lines is not None
                    and identity and (len(ops) == 1 or self.store.state(f'hour_media:{ident}'))
                    and all(key in payload for key in ('local_id', 'sort_seq', 'shard', 'create_time')))
        if not eligible:
            if saved:
                raise Unavailable('未确认的小时卡片无法完整重建，未重新发送')
            self.break_conversation(pair)
            return ops, None
        group = self.pairs[pair]['wx_group']
        actor, stamp = identity(group, payload)[1], int(payload['create_time'])
        if not actor or stamp <= 0:
            if saved:
                raise Unavailable('未确认的小时卡片发送者或时间无法核对，未重新发送')
            self.break_conversation(pair)
            return ops, None
        clock = time.localtime(stamp)
        hour = stamp - clock.tm_min * 60 - clock.tm_sec
        previous = self.conversations.get(pair)
        if not previous or 'hour' not in previous:
            previous = self.store.state(pair + ':hour_card')
        parts = [json.loads(row[0]) for row in self.store.conn.execute('SELECT value FROM state WHERE key LIKE ?',
                 (pair + ':hour_card:' + str(hour) + ':%',)).fetchall()]
        if previous and previous.get('hour') == hour and not any(p['message_id'] == previous['message_id'] for p in parts):
            parts.append(previous)
        if not saved and parts:
            parts.sort(key=lambda part: part['part'])
            previous = next((part for part in parts if stamp <= part['time']), parts[-1])
        locator = dict({key: payload[key] for key in ('local_id', 'sort_seq', 'shard', 'type')},
                       hash=fingerprint(ops), actor=hashlib.sha256(actor.encode()).hexdigest(), time=stamp)
        locator['turn'] = bool(saved['sources'][-1].get('turn')) if saved else bool(previous and previous.get('gap'))
        if saved:
            locator['turn'] = bool(saved.get('current', saved['sources'][-1]).get('turn'))
        locator['echo'] = self.store.native_echo(pair, task['key'])
        if task['key'].startswith('wx_card:') and not locator['echo']:
            locator['summary_only'] = True
        context = {'hour': hour, 'part': 1, 'time': stamp, 'sources': [locator]}
        if saved:
            expected = saved.get('current', saved['sources'][-1])
            if 'echo' not in expected:
                expected = dict(expected, echo=False)
            if saved['hour'] != hour or expected != locator:
                raise Unavailable('未确认的小时卡片原消息发生变化，未重新发送')
            context.update(saved)
        elif previous and previous.get('hour') == hour:
            context['part'] = previous['part']
            if not previous.get('blocked'):
                original = self.fs.get_message(previous['message_id'])
                if any(item.get('message_id') == previous['message_id'] and item.get('chat_id') == self.pairs[pair]['fs_group']
                       and item.get('msg_type') == 'interactive' and not item.get('deleted') for item in original):
                    sources = [source for source in previous['sources'] if (source['local_id'], source['sort_seq'], source['shard'])
                               != (locator['local_id'], locator['sort_seq'], locator['shard'])] + [locator]
                    sources.sort(key=lambda source: (int(source['sort_seq']), source['shard'], int(source['local_id'])))
                    context.update(sources=sources, message_id=previous['message_id'])
            if 'message_id' not in context:
                context['part'] = max(part['part'] for part in parts) + 1

        def render(current):
            return self.render_hour_context(pair, current, ident)

        rendered = render(context)
        preview = hour_card_content(rendered)
        if len(preview['body']['elements']) == 1:
            return ops, None
        if not card_fits(preview):
            if saved:
                raise Unavailable('小时卡片超出飞书容量，未修改原卡片')
            if 'message_id' not in context:
                self.break_conversation(pair)
                return ops, None
            context = dict(context, part=max([context['part']] + [part['part'] for part in parts]) + 1, sources=[locator])
            context.pop('message_id', None)
            rendered = render(context)
            if not card_fits(hour_card_content(rendered)):
                self.break_conversation(pair)
                return ops, None
        context.update(current=locator, time=max(source['time'] for source in context['sources']))
        if 'message_id' in context:
            digest = fingerprint(context['sources'])
            if saved and saved['hash'] != digest:
                raise Unavailable('小时卡片的原消息指纹发生变化，未修改卡片')
            context['hash'] = digest
            self.store.set_state(f'card_merge:{ident}', context)
            rendered['append_to'] = context['message_id']
        native = ops[1:]
        if native and self.store.state(f'cloud_media:{ident}'):
            native = [{'kind': 'cloud_receipt'}]
        elif native and hour_paragraphs(ops)[-1][0].get('media_type') == 'video':
            native = [{'kind': 'post', 'paragraphs': [[{'tag': 'media', 'path': native[0]['path']}]]}]
        return [rendered] + native, context

    def daily_conversation_ops(self, task, ops):
        from feishu import card_fits, hour_card_content
        pair, ident = task['pair'], task['id']
        saved = self.store.state(f'card_merge:{ident}')
        payload = json.loads(self.store.conn.execute('SELECT payload FROM task WHERE id=?', (ident,)).fetchone()[0])
        lines = hour_paragraphs(ops) if task['direction'] == 'wx_to_fs' else None
        eligible = (lines is not None and getattr(self.wx, 'message_identity', None)
                    and (len(ops) == 1 or self.store.state(f'hour_media:{ident}'))
                    and all(key in payload for key in ('local_id', 'sort_seq', 'shard', 'create_time')))
        if not eligible:
            if saved:
                raise Unavailable('日卡片的原消息无法完整重建，未重新发送')
            return ops, None
        actor, stamp = self.wx.message_identity(self.pairs[pair]['wx_group'], payload)[1], int(payload['create_time'])
        if not actor or stamp <= 0:
            raise Unavailable('日卡片发送者或时间无法核对，未发送')
        clock = time.localtime(stamp)
        day = int(time.mktime((clock.tm_year, clock.tm_mon, clock.tm_mday, 0, 0, 0, 0, 0, -1)))
        locator = dict({name: payload[name] for name in ('local_id', 'sort_seq', 'shard', 'type')},
            time=stamp, hash=fingerprint(ops), actor=hashlib.sha256(actor.encode()).hexdigest(),
            turn=False, echo=self.store.native_echo(pair, task['key']))
        if task['key'].startswith('wx_card:') and not locator['echo']:
            locator['summary_only'] = True
        if saved:
            if saved['hour'] != day or saved['current'] != locator:
                raise Unavailable('未确认的日卡片原消息发生变化，未重新发送')
            context = saved
        else:
            parts = [json.loads(row[0]) for row in self.store.conn.execute('SELECT value FROM state WHERE key LIKE ?',
                (pair + ':day_card:' + str(day) + ':%',)).fetchall()]
            parts.sort(key=lambda part: part['part'])
            previous = next((part for part in parts if stamp <= part['time']), parts[-1] if parts else None)
            part = previous['part'] if previous else 1
            if previous and previous.get('blocked'):
                # Preserve the unknown update; a new message can start a verified continuation.
                previous, part = None, max(p['part'] for p in parts) + 1
            slot = self.store.state(self.day_slot(pair, {'hour': day, 'part': part}))
            if slot and slot.get('phase') != 'confirmed' and slot.get('task_id') != ident:
                raise Unavailable('当天卡片上次创建结果尚未核对，已保留新消息，避免重复创建')
            if previous:
                sources = [dict(source) for source in previous['sources']]
                old_key = lambda source: (int(source['sort_seq']), source['shard'], int(source['local_id']))
                keys = [old_key(source) for source in sources]
                index = bisect.bisect_left(keys, old_key(locator))
                if index < len(sources) and keys[index] == old_key(locator):
                    # A backfill may target the same locator; it never creates another day card.
                    sources[index] = locator
                else:
                    sources.insert(index, locator)
                context = dict(previous, sources=sources, pages=[len(sources)], page=1, current=locator)
                context.pop('blocked', None)
            else:
                context = {'period': 'day', 'hour': day, 'part': part, 'time': stamp,
                           'sources': [locator], 'pages': [1], 'page': 1, 'current': locator}
            # Filled cards stay in chat history; only the current continuation is rebuilt.
            rendered = self.render_hour_context(pair, context, ident)
            if not card_fits(hour_card_content(rendered)):
                if not context.get('message_id'):
                    raise Unavailable('这条消息超出卡片容量，已保留原消息，未截断内容')
                context = {'period': 'day', 'hour': day, 'part': max(p['part'] for p in parts) + 1,
                    'time': stamp, 'sources': [locator], 'pages': [1], 'page': 1, 'current': locator}
                rendered = self.render_hour_context(pair, context, ident)
                if not card_fits(hour_card_content(rendered)):
                    raise Unavailable('这条消息超出卡片容量，已保留原消息，未截断内容')
                slot = self.store.state(self.day_slot(pair, context))
                if slot and slot.get('phase') != 'confirmed' and slot.get('task_id') != ident:
                    raise Unavailable('当天续卡创建结果尚未核对，未重复创建')
            context['time'] = max(source['time'] for source in context['sources'])
            self.store.set_state(f'card_merge:{ident}', context)
        rendered = self.render_hour_context(pair, context, ident)
        if not card_fits(hour_card_content(rendered)):
            raise Unavailable('日卡片当前页超出容量，原消息保留')
        if context.get('message_id'):
            rendered['append_to'] = context['message_id']
        native = ops[1:]
        if native and self.store.state(f'cloud_media:{ident}'):
            native = [{'kind': 'cloud_receipt'}]
        elif native and lines[-1][0].get('media_type') == 'video':
            native = [{'kind': 'post', 'paragraphs': [[{'tag': 'media', 'path': native[0]['path']}]]}]
        return [rendered] + native, context

    @staticmethod
    def day_slot(pair, context):
        return pair + ':day_slot:' + str(context['hour']) + (':' + str(context['part']) if context['part'] > 1 else '')

    def break_conversation(self, pair, blocked=False):
        if self.daily_cards:
            # Unknown patches block their own day; unrelated days and groups may proceed.
            return
        previous = self.conversations.pop(pair, None)
        if not previous or 'hour' not in previous:
            previous = self.store.state(pair + ':hour_card')
        if previous and 'hour' in previous:
            previous = dict(previous, gap=True, blocked=blocked or previous.get('blocked', False))
            self.store.set_state(pair + ':hour_card', previous)
            self.store.set_state(pair + ':hour_card:' + str(previous['hour']) + ':' + str(previous['part']), previous)

    def remember_hour(self, pair, ident, context, message_id):
        latest = self.store.conn.execute("SELECT max(id) FROM task WHERE pair=? AND direction='wx_to_fs' AND status='done'", (pair,)).fetchone()[0] or 0
        context = dict(context, message_id=message_id, seq=int(context['sources'][-1]['sort_seq']))
        if context.get('period') == 'day':
            start = context['pages'][context['page'] - 2] if context['page'] > 1 else 0
            for source in context['sources'][start:context['pages'][context['page'] - 1]]:
                source['verified'] = message_id
            context.pop('blocked', None)
            self.store.set_state(pair + ':day_card:' + str(context['hour']) + ':' + str(context['part']), context)
            self.store.set_state(self.day_slot(pair, context),
                                 {'phase': 'confirmed', 'message_id': message_id, 'task_id': ident})
            active = self.store.state(pair + ':day_card')
            if not active or (context['hour'], context['part']) >= (active['hour'], active['part']):
                self.store.set_state(pair + ':day_card', context)
            return
        self.store.set_state(pair + ':hour_card:' + str(context['hour']) + ':' + str(context['part']), context)
        active = self.store.state(pair + ':hour_card')
        if ident >= latest and (not active or (context['hour'], context['part']) >= (active['hour'], active['part'])):
            self.conversations[pair] = context
            self.store.set_state(pair + ':hour_card', context)

    def legacy_conversation_ops(self, task, ops):
        pair, ident = task['pair'], task['id']
        payload = json.loads(self.store.conn.execute('SELECT payload FROM task WHERE id=?', (ident,)).fetchone()[0])
        saved = self.store.state(f'card_merge:{ident}')
        identity = getattr(self.wx, 'message_identity', None)
        eligible = (task['direction'] == 'wx_to_fs' and payload.get('type') in ('文本', '图片')
                    and identity and len(ops) == 1 and ops[0]['kind'] == 'post' and not ops[0].get('reply_to')
                    and all(key in payload for key in ('local_id', 'sort_seq', 'shard', 'create_time')))
        if eligible:
            for path in media_paths(ops):
                with Path(path).open('rb') as resource:
                    if resource.read(6) in (b'GIF87a', b'GIF89a'):
                        eligible = False
        if not eligible:
            if saved:
                raise Unavailable('未确认的合并会话无法完整重建，未重新发送')
            self.conversations.pop(pair, None)
            return ops, None
        group = self.pairs[pair]['wx_group']
        actor = identity(group, payload)[1]
        stamp = int(payload['create_time'])
        if not actor or stamp <= 0:
            if saved:
                raise Unavailable('未确认的合并会话发送者或时间无法核对，未重新发送')
            self.conversations.pop(pair, None)
            return ops, None
        actor_hash = hashlib.sha256(actor.encode()).hexdigest()
        locator = dict({key: payload[key] for key in ('local_id', 'sort_seq', 'shard', 'type')}, hash=fingerprint(ops))
        context = {'actor': actor_hash, 'time': stamp, 'sources': [locator]}
        previous = self.conversations.get(pair)
        if saved:
            if saved['actor'] != actor_hash or saved['time'] != stamp or saved['sources'][-1] != locator:
                raise Unavailable('未确认的合并会话原消息发生变化，未重新发送')
            context.update(saved)
        elif (previous and previous['actor'] == actor_hash and 0 <= stamp - previous['time'] <= 60
              and int(payload['sort_seq']) >= previous['seq']):
            original = self.fs.get_message(previous['message_id'])
            if any(item.get('message_id') == previous['message_id'] and item.get('chat_id') == self.pairs[pair]['fs_group']
                   and item.get('msg_type') == 'interactive' and not item.get('deleted') for item in original):
                context.update(sources=previous['sources'] + [locator], message_id=previous['message_id'])
        if 'message_id' not in context:
            return [dict(ops[0], conversation_card=True, separate_all=True)], context
        lines = []
        for source in context['sources']:
            message = self.wx.db.reload_message(group, source)
            if message.get('type') not in ('文本', '图片') or hashlib.sha256(identity(group, message)[1].encode()).hexdigest() != actor_hash:
                raise Unavailable('合并会话的原消息或发送者发生变化，未修改卡片')
            converted = post_ops(self.wx.to_feishu(group, message))
            self.store.register_files(ident, converted)
            if len(converted) != 1 or converted[0]['kind'] != 'post' or fingerprint(converted) != source['hash']:
                raise Unavailable('合并会话的原消息发生变化或无法完整重建，未修改卡片')
            lines.extend(converted[0]['paragraphs'])
        # 卡片组件有额外开销，预留容量避免追加时退化为一条重复的富文本。
        if post_size(lines) + 1024 + 256 * len(lines) > MAX_POST or len(lines) > 90:
            if saved:
                raise Unavailable('合并会话超出卡片容量，未修改卡片')
            return [dict(ops[0], conversation_card=True, separate_all=True)], {'actor': actor_hash, 'time': stamp, 'sources': [locator]}
        merged = dict(ops[0], paragraphs=lines)
        digest = fingerprint(merged)
        if saved and saved['hash'] != digest:
            raise Unavailable('合并会话的原消息发生变化，未修改卡片')
        context['hash'] = digest
        self.store.set_state(f'card_merge:{ident}', context)
        return [dict(merged, conversation_card=True, separate_all=True, append_to=context['message_id'])], context

    def deliver(self, task):
        ident = task["id"]
        pair = task['pair']
        binding = self.pairs[pair]
        wx_group, fs_group = binding['wx_group'], binding['fs_group']
        started = time.monotonic()
        timings = {'task_id': ident, 'direction': task['direction']}
        try:
            self.register_wx_files(task)
            if self.store.state(f'media_wait:{ident}'):
                self.store.set_state(f'media_wait:{ident}', False)
            ops = self.prepare_ops(task)
            timings['prepare_s'] = round(time.monotonic() - started, 3)
            if not ops:
                return
            plan = fingerprint(ops)
            previous = self.store.state(f'plan:{ident}')
            if previous and previous != plan:
                raise Unavailable('原消息或附件发生变化，请先核对已发送内容；未重发')
            if (previous is None and task['direction'] == 'wx_to_fs' and len(ops) == 2
                    and hour_paragraphs(ops) is not None):
                self.store.set_state(f'hour_media:{ident}', True)
                if self.cloud_files:
                    self.store.set_state(f'cloud_media:{ident}', True)
            self.store.set_state(f'plan:{ident}', plan)
            self.store.register_files(ident, ops)
            self.register_wx_files(task)
            if self.stop.is_set():
                return
            alert = (task['direction'] in ('wx_to_fs', 'wx_batch_to_fs')
                     and not self.store.native_echo(pair, task['key'])) and any(
                node.get('tag') == 'at' and node.get('user_id') == 'all'
                for op in ops if op['kind'] == 'post' for line in op['paragraphs'] for node in line)
            alert_id = self.store.state(f'all_notice:{ident}')
            if alert and not alert_id and task['step'] == 0:
                members = sorted(self.fs.members(fs_group))
                notice = self.store.state(f'member_notice:{ident}') or {'targets': members, 'sent': []}
                if not members or notice['targets'] != members:
                    raise Unavailable('飞书群成员为空或通知期间成员有变化，请核对已发送通知后重试')
                self.store.set_state(f'member_notice:{ident}', notice)
                for part in range(len(notice['sent']), (len(members) + 49) // 50):
                    if self.stop.is_set():
                        return
                    self.store.update(ident, status='sending')
                    notification = {'kind': 'post', 'member_notice': True, 'paragraphs': [[
                        {'tag': 'at', 'user_id': member} for member in members[part * 50:(part + 1) * 50]]]}
                    message_id = self.fs.send(fs_group, notification, pair + ':' + task['key'] + ':members:' + str(part))
                    if not message_id:
                        raise Uncertain('飞书成员通知未返回发送确认，未自动重发')
                    self.store.add_echo('fs:' + message_id)
                    notice['sent'].append(message_id)
                    self.store.set_state(f'member_notice:{ident}', notice)
                    self.store.update(ident, status='pending')
                alert_id = notice['sent'][0]
                self.store.set_state(f'all_notice:{ident}', alert_id)
            rendering = time.monotonic()
            rendered, conversation = self.conversation_ops(task, ops)
            timings['render_s'] = round(time.monotonic() - rendering, 3)
            video_post = (task['direction'] == 'wx_to_fs' and task['step'] == 0 and len(ops) == 2
                          and not (conversation and 'hour' in conversation)
                          and ops[0]['kind'] == 'post' and ops[1]['kind'] in ('video', 'file')
                          and Path(ops[1]['path']).suffix.lower() == '.mp4'
                          and (not previous or self.store.state(f'video_post:{ident}')))
            if video_post and ops[1]['kind'] == 'file':
                with Path(ops[1]['path']).open('rb') as video:
                    video_post = video.read(12)[4:8] == b'ftyp'
            if video_post:
                self.store.set_state(f'video_post:{ident}', True)
                lines = ops[0]['paragraphs']
                if len(lines) == 1 and len(lines[0]) == 1:
                    node = lines[0][0]
                    header = node.get('sender', '') + '：'
                    if node.get('sender') and node.get('text') == header + Path(ops[1]['path']).name:
                        lines = [[dict(node, text=header)]]
                rendered = [dict(ops[0], paragraphs=lines + [[{'tag': 'media', 'path': ops[1]['path']}]]), ops[1]]
            sent_kind = ''
            for index in range(task["step"], len(ops)):
                if self.stop.is_set():
                    return
                if video_post and index == 1:
                    continue
                op = rendered[index]
                if op['kind'] == 'cloud_receipt':
                    target = self.store.state(f'sent:{ident}:0')
                    if not target:
                        raise Unavailable('云盘附件所在的小时卡片尚未确认，未标记完成')
                    self.store.set_state(f'sent:{ident}:{index}', target)
                    self.store.update(ident, step=index + 1, status='pending')
                    continue
                if op['kind'] == 'record_media':
                    if task['direction'] != 'wx_to_fs':
                        raise Unavailable('合并记录附件不属于微信转发任务，已保留')
                    target = self.store.state(f'sent:{ident}:0')
                    if not target:
                        raise Unavailable('合并记录正文尚未确认，附件已保留')
                    item = op['item']
                    media = {'kind': item['kind'], 'path': self.wx.record_media(wx_group, item)}
                    op = post_ops([{'kind': 'text', 'text': item['sender'] + '：', 'sender': item['sender']}, media])[0] if item['kind'] == 'image' else media
                    op['reply_to'] = target
                    self.store.register_files(ident, op)
                self.store.update(ident, status="sending")
                if task["direction"] in ("fs_to_wx", "card_to_wx"):
                    revision = self.store.state(f'wx_retry:{ident}', 0)
                    attempt = pair + ':' + task['key'] + ':' + str(index) + ':' + str(revision)
                    self.wx.send(wx_group, ops[index], self.store, attempt)
                    acknowledgement = self.store.state('wx_attempt:' + attempt)
                    if acknowledgement and acknowledgement.get('ack'):
                        self.mirror_message(pair, acknowledgement['ack'])
                elif task['direction'] == 'notice_to_fs':
                    self.fs.set_announcement(fs_group, ops[index]['text'], pair + ':' + task['key'])
                else:
                    if (task['direction'] == 'wx_to_fs' and op['kind'] == 'post'
                            and not (conversation and 'hour' in conversation)):
                        payload = json.loads(self.store.conn.execute('SELECT payload FROM task WHERE id=?', (ident,)).fetchone()[0])
                        avatar = getattr(self.wx, 'avatar_path', lambda *args: None)(wx_group, payload)
                        if avatar:
                            op = dict(op, avatar={'path': avatar})
                            self.store.register_files(ident, op)
                    if alert and op['kind'] in ('post', 'hour_card'):
                        op = dict(op, separate_all=True, all_message_id=alert_id)
                    if (task['direction'] == 'wx_to_fs' and op['kind'] == 'post'
                            and index + 1 < len(ops) and ops[index + 1]['kind'] == 'file'):
                        # 仅调整发送时的显示，保留旧任务的指纹和附件续发位置。
                        op = dict(op, attachment_name=Path(ops[index + 1]['path']).name)
                    daily = conversation and conversation.get('period') == 'day'
                    creating = daily and not op.get('append_to')
                    if daily:
                        slot_key = self.day_slot(pair, conversation)
                        if creating:
                            self.store.set_state(slot_key, {'phase': 'started', 'task_id': ident})
                    message_id = self.fs.send(fs_group, op, pair + ":" + task["key"] + ":" + str(index))
                    sent_kind = getattr(self.fs, 'last_sent_kind', '')
                    if not message_id:
                        raise Uncertain("飞书未返回发送确认，未自动重发")
                    self.store.add_echo("fs:" + message_id)
                    if daily:
                        conversation['message_id'] = message_id
                        self.store.set_state(f'card_merge:{ident}', conversation)
                        if creating:
                            self.store.set_state(slot_key, {'phase': 'created', 'task_id': ident, 'message_id': message_id})
                        self.fs.verify_hour_card(fs_group, op, message_id)
                    self.store.set_state(f'sent:{ident}:{index}', message_id)
                    if conversation and 'hour' in conversation and sent_kind == 'interactive':
                        self.remember_hour(pair, ident, conversation, message_id)
                    if video_post:
                        self.store.set_state(f'sent:{ident}:1', message_id)
                    if task['direction'] == 'wx_to_fs' and index == 0:
                        source = self.store.state(f'wx_source:{ident}')
                        if source:
                            self.store.set_state(pair + ':wx_sent:' + source, message_id)
                self.store.update(ident, step=index + (2 if video_post else 1), status="pending")
            self.store.complete(ident)
            latest = self.store.conn.execute("SELECT max(id) FROM task WHERE pair=? AND direction='wx_to_fs' AND status='done'", (pair,)).fetchone()[0]
            if (conversation and 'hour' in conversation or
                    task['direction'] in ('fs_to_wx', 'card_to_wx')):
                pass
            elif conversation and sent_kind == 'interactive' and ident >= latest:
                # shortcut: 重启后新开一段；未确认的追加仍由 card_merge 定位原卡片。
                self.conversations[pair] = dict(conversation, message_id=message_id, seq=int(conversation['sources'][-1]['sort_seq']))
            else:
                self.break_conversation(pair)
            self.log(f"已同步 #{ident} · {binding.get('name', wx_group)} · " + (
                '飞书群公告已更新' if task['direction'] == 'notice_to_fs' else
                "飞书 → 微信" if task["direction"] in ("fs_to_wx", "card_to_wx") else "微信 → 飞书"))
        except Exception as exc:
            if isinstance(exc, ConfirmationPending):
                self.store.update(ident, status='pending', error='')
                self.awaiting[ident] = time.monotonic() + 1
                timings['awaiting_wechat_confirmation'] = True
                return
            row = self.store.conn.execute("SELECT status FROM task WHERE id=?", (ident,)).fetchone()
            uncertain = isinstance(exc, Uncertain) or (row[0] == "sending" and not isinstance(exc, Unavailable))
            if uncertain and (pending := self.store.state(f'card_merge:{ident}')) and pending.get('period') == 'day':
                existing = self.store.state(pair + ':day_card:' + str(pending['hour']) + ':' + str(pending['part']))
                if existing:
                    self.store.set_state(pair + ':day_card:' + str(pending['hour']) + ':' + str(pending['part']), dict(existing, blocked=True))
            self.break_conversation(pair, blocked=uncertain)
            error = str(exc) if isinstance(exc, (Unavailable, Uncertain)) else type(exc).__name__ + "：处理未完成"
            if isinstance(exc, MediaPending) and task['direction'] == 'wx_to_fs' and not uncertain:
                self.store.set_state(f'media_wait:{ident}', True)
            self.store.update(ident, status="uncertain" if uncertain else "failed", error=error)
            if error != task['error']:
                self.log(f"#{ident} 未同步：{error}")
            if isinstance(exc, PermissionDenied) and task['direction'] != 'notice_to_fs':
                raise
        finally:
            timings['total_s'] = round(time.monotonic() - started, 3)
            self.latency[task['direction']] = timings
            self.register_wx_files(task)
            self.store.cleanup_completed_files()

    def register_wx_files(self, task):
        if task['direction'] != 'wx_to_fs':
            return
        payload = json.loads(task['payload'])
        if 'local_id' not in payload or 'sort_seq' not in payload:
            return
        source = wx_key(self.pairs[task['pair']]['wx_group'], payload)
        digests = (hashlib.sha256(source.encode()).hexdigest(), hashlib.sha256(('video:' + source).encode()).hexdigest())
        for digest in digests:
            folder = self.directory / 'wechat/media' / digest
            self.store.register_files(task['id'], [{'path': str(path)} for path in folder.glob('*') if path.is_file()])
        if payload.get('type') == '视频':
            partial = self.directory / 'feishu/media/wechat-video' / (digests[1] + '.mp4.part')
            self.store.register_files(task['id'], {'path': str(partial)})

    def sync_wechat(self, notices_due, media_due):
        with getattr(self.wx.db, "snapshot_scope", nullcontext)():
            # Interactive replies go first; a media/history backlog cannot sit in front of them.
            self.drain_feishu()
            if media_due and self.daily_cards:
                self.recover_card_readbacks()
            for pair in self.pairs:
                task = self.ready_task(pair)
                if task is not None and task['direction'] in ('fs_to_wx', 'card_to_wx') and not self.stop.is_set():
                    self.deliver(task)
            for pair, binding in self.pairs.items():
                if self.stop.is_set():
                    break
                self.poll_wechat(pair)
                if media_due:
                    self.store.retry_media(pair)
                if notices_due:
                    try:
                        notice = self.wx.notice(binding['wx_group'])
                        if notice:
                            self.store.enqueue(pair, "wx_announcement:" + notice["version"], "notice_to_fs",
                                               {"content": notice["content"]})
                    except Exception:
                        self.log(binding.get('name', binding['wx_group']) + "：微信群公告暂不可读取，其他消息继续同步")
                task = self.ready_task(pair)
                if task is not None:
                    self.deliver(task)

    def recover_card_readbacks(self):
        # Reconcile only known card IDs; this cannot send another native or Feishu message.
        rows = self.store.conn.execute("SELECT id,pair FROM task WHERE direction='wx_to_fs' AND status='uncertain' AND step=0 ORDER BY id").fetchall()
        for row in rows:
            pair, ident = row['pair'], row['id']
            context = self.store.state(f'card_merge:{ident}')
            if pair not in self.pairs or not context or context.get('period') != 'day' or not context.get('message_id'):
                continue
            try:
                op = self.render_hour_context(pair, context, ident)
                target = context['message_id']
                self.fs.verify_hour_card(self.pairs[pair]['fs_group'], op, target)
                self.store.set_state(f'sent:{ident}:0', target)
                self.remember_hour(pair, ident, context, target)
                if (source := self.store.state(f'wx_source:{ident}')):
                    self.store.set_state(pair + ':wx_sent:' + source, target)
                self.store.update(ident, step=1, status='pending', error='')
                guard_errors = ('当天卡片上次创建结果尚未核对，已保留新消息，避免重复创建',
                                '当天卡片上次更新结果尚未核对，请先处理需核对的消息')
                unsent_guards = """SELECT id FROM task WHERE pair=? AND direction='wx_to_fs'
                    AND status='failed' AND step=0 AND error IN (?,?)
                    AND NOT EXISTS (SELECT 1 FROM state WHERE key='card_merge:' || task.id)
                    AND NOT EXISTS (SELECT 1 FROM state WHERE key LIKE 'sent:' || task.id || ':%')"""
                # These messages never reached send(); reload their current source before their first send.
                self.store.conn.execute('DELETE FROM state WHERE key IN (SELECT \'plan:\' || id FROM (' + unsent_guards + '))',
                                        (pair, *guard_errors))
                self.store.conn.execute("""UPDATE task SET status='pending',error='',ops=NULL,
                    payload=source_locator(payload,direction,status)
                    WHERE pair=? AND direction='wx_to_fs' AND status='failed' AND step=0
                    AND error IN (?,?) AND NOT EXISTS (SELECT 1 FROM state WHERE key='card_merge:' || task.id)
                    AND NOT EXISTS (SELECT 1 FROM state WHERE key LIKE 'sent:' || task.id || ':%')""",
                    (pair, *guard_errors))
                self.store.conn.commit()
                self.log(f'#{ident}：卡片回读已确认，继续处理后续消息')
            except (Unavailable, Uncertain, RuntimeError, OSError, ValueError, KeyError):
                continue

    def ready_task(self, pair):
        now = time.monotonic()
        self.awaiting = {ident: deadline for ident, deadline in self.awaiting.items() if deadline > now}
        return self.store.next_task(pair, tuple(self.awaiting))

    def drain_feishu(self):
        while not self.fs.events.empty():
            event = self.fs.events.get_nowait()
            self.receive_fs(event)
            event = None

    def run(self):
        import pythoncom
        pythoncom.CoInitialize()
        reason = ""
        stage = "打开本机队列"
        try:
            self.store = Store(self.directory)
            for pair, binding in self.pairs.items():
                if self.stop.is_set():
                    return
                if self.store.state(pair + ":fs_floor") is None:
                    self.store.set_state(pair + ":fs_floor", int(time.time() * 1000))
                stage = "检查机器人入群：" + binding.get('name', binding['wx_group'])
                self.emit('status', stage)
                self.fs.ensure_bot(binding['fs_group'])
            stage = "检查飞书群消息读取权限"
            self.emit('status', stage)
            self.catch_up()
            if self.stop.is_set():
                return
            stage = "连接飞书消息监听"
            self.emit('status', stage)
            self.fs.start_events(list(self.fs_pairs))
            if self.daily_cards:
                for pair in self.pairs:
                    mirror = self.store.conn.execute("SELECT * FROM task WHERE pair=? AND direction='wx_to_fs' AND status='pending' AND key LIKE 'wx_card:%' ORDER BY id LIMIT 1", (pair,)).fetchone()
                    if mirror:
                        self.deliver(mirror)
            try:
                if getattr(self.wx, 'connect_recaller', lambda groups: False)([b['wx_group'] for b in self.pairs.values()]):
                    self.log('测试群后台撤回已启用；撤回结果仅写入软件日志')
            except (RuntimeError, OSError, ValueError):
                self.log('测试群撤回组件暂未连接，普通消息同步继续运行')
            self.catch_up()
            for pair, binding in self.pairs.items():
                if self.history:
                    stage = "读取本机微信历史：" + binding.get('name', binding['wx_group'])
                    self.import_history(pair)
                if self.stop.is_set():
                    return
                with getattr(self.wx.db, 'snapshot_scope', nullcontext)():
                    self.backfill_replies(pair)
                if self.store.state(pair + ':wx_cursor') is None:
                    recent = self.wx.db.get_messages(binding['wx_group'], limit=1)
                    cursor = max((int(m['sort_seq']) for m in recent), default=0)
                    limit = 1000
                    while True:
                        baseline = self.wx.db.get_new_messages(binding['wx_group'], since_seq=cursor - 1, limit=limit)
                        for message in baseline:
                            if int(message['sort_seq']) <= cursor:
                                self.store.add_echo(wx_key(binding['wx_group'], message))
                        if len(baseline) < limit or any(int(m['sort_seq']) > cursor for m in baseline) or self.stop.is_set():
                            break
                        limit *= 2
                    self.store.set_state(pair + ':wx_cursor', cursor)
            generation = self.fs.generation
            last_history, last_notice, last_media = time.monotonic(), 0, 0
            self.log(f"微信 ⇄ 飞书后台同步已启动：{len(self.pairs)} 对群；支持文字、图片、文件和 @，不操作窗口和剪贴板")
            self.emit("started", None)
            self.emit("status", f"正在后台双向同步 {len(self.pairs)} 对群；微信无需前台，不占剪贴板")
            stage = "同步消息"
            while not self.stop.is_set():
                if self.fs.process is None or self.fs.process.poll() is not None:
                    raise Unavailable("飞书监听退出，同步已停止；修复授权或网络后重新开始会补取消息")
                while not self.commands.empty():
                    command, ident = self.commands.get_nowait()
                    row = self.store.conn.execute('SELECT pair FROM task WHERE id=?', (ident,)).fetchone()
                    if row and row['pair'] in self.pairs:
                        if command == 'retry':
                            self.store.retry(ident)
                        else:
                            self.store.complete(ident)
                if generation != self.fs.generation or time.monotonic() - last_history >= 3600:
                    self.catch_up()
                    generation = self.fs.generation
                    last_history = time.monotonic()
                self.drain_feishu()
                actions = getattr(self.fs, 'recall_actions', None)
                while actions is not None and not actions.empty() and not self.stop.is_set():
                    self.recall_action(actions.get_nowait())
                notices_due = self.notices and time.monotonic() - last_notice >= 60
                media_due = time.monotonic() - last_media >= 30
                self.sync_wechat(notices_due, media_due)
                if notices_due:
                    last_notice = time.monotonic()
                if media_due:
                    last_media = time.monotonic()
                self.report_stats()
                self.stop.wait(.2)
        except Exception as exc:
            reason = stage + "：" + (str(exc) if isinstance(exc, (Unavailable, Uncertain)) else type(exc).__name__)
            self.log("同步停止：" + reason)
        finally:
            caller = getattr(self.wx, 'recaller', None)
            if caller is not None:
                try:
                    caller.close()
                except (OSError, RuntimeError):
                    self.log('测试群撤回组件暂未确认停止；微信正常退出后会释放')
                self.wx.recaller = None
            try:
                self.fs.close()
            except Unavailable as exc:
                self.log(str(exc))
            if hasattr(self, "store"):
                self.report_stats()
                self.store.conn.close()
            getattr(self.wx.db, "release", lambda: None)()
            pythoncom.CoUninitialize()
            self.emit("stopped", reason)



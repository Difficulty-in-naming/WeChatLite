"""只在内存中解密微信库；源数据库和 WAL 始终只读。"""
from contextlib import contextmanager
import os
from pathlib import Path
import sqlite3
import struct
import threading
import weakref

from wechatauto.db import WeChatDB, PAGE_SZ, _decrypt_page, _sqlite_text_factory


def checksum(data, order, previous=(0, 0)):
    first, second = previous
    for left, right in struct.iter_unpack(order + 'II', data):
        first = (first + left + second) & 0xffffffff
        second = (second + right + first) & 0xffffffff
    return first, second


def committed_frames(wal):
    if len(wal) < 32:
        return [], None
    magic, version, page_size = struct.unpack('>III', wal[:12])
    if magic not in (0x377f0682, 0x377f0683) or version != 3007000 or page_size != PAGE_SZ:
        raise RuntimeError('微信 WAL 头无效，未推进同步进度')
    order = '<' if magic == 0x377f0682 else '>'
    running = checksum(wal[:24], order)
    if running != struct.unpack('>II', wal[24:32]):
        raise RuntimeError('微信 WAL 头校验失败，未推进同步进度')
    frames, committed, size = [], 0, None
    for offset in range(32, len(wal) - (24 + PAGE_SZ) + 1, 24 + PAGE_SZ):
        header = wal[offset:offset + 24]
        page = wal[offset + 24:offset + 24 + PAGE_SZ]
        number, commit_size = struct.unpack('>II', header[:8])
        if not number or header[8:16] != wal[16:24]:
            break
        running = checksum(page, order, checksum(header[:8], order, running))
        if running != struct.unpack('>II', header[16:24]):
            break
        frames.append((number, page))
        if commit_size:
            committed, size = len(frames), commit_size
    return frames[:committed], size


class MemoryConnection(sqlite3.Connection):
    pass


def protobuf_bytes(data):
    if not isinstance(data, bytes) or len(data) > 8_000_000:
        raise ValueError('微信群昵称数据无效')
    offset = 0

    def varint():
        nonlocal offset
        value = 0
        for shift in range(0, 70, 7):
            if offset >= len(data):
                break
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if not byte & 128 and value < 1 << 64:
                return value
        raise ValueError('微信群昵称数据截断')

    while offset < len(data):
        key = varint()
        field, wire = key >> 3, key & 7
        if not field:
            raise ValueError('微信群昵称字段无效')
        if wire == 0:
            varint()
            continue
        length = varint() if wire == 2 else {1: 8, 5: 4}.get(wire)
        if length is None or offset + length > len(data):
            raise ValueError('微信群昵称字段截断')
        if wire == 2:
            yield field, data[offset:offset + length]
        offset += length


class MemoryWeChatDB(WeChatDB):
    def __init__(self, **kwargs):
        self._snapshots = {}
        self._message_senders = {}
        self._group_names = {}
        self._connections = weakref.WeakSet()
        self._scope = threading.local()
        super().__init__(**kwargs)

    @contextmanager
    def snapshot_scope(self):
        self._scope.active = True
        self._scope.message_connections = None
        try:
            yield
        finally:
            self._scope.active = False
            self.release()

    def release(self):
        self._snapshots.clear()
        self._message_senders.clear()
        self._group_names.clear()
        for conn in list(self._connections):
            if conn.owner == threading.get_ident():
                conn.close()
                self._connections.discard(conn)
        self._scope.message_connections = None

    def _invalidate_cache(self):
        self._snapshots.clear()
        self._message_senders.clear()
        self._group_names.clear()

    def _run_msg_query(self, user, build):
        if not getattr(self._scope, 'active', False):
            return super()._run_msg_query(user, build)
        # One snapshot/connection per shard serves every watched group in this polling round.
        if getattr(self._scope, 'message_connections', None) is None:
            self._scope.message_connections = [self._open(rel) for rel in self._message_dbs()]
        found = self._find_msg_tables(user, self._scope.message_connections)
        return build(found) if found else None

    def _msg_row_to_dict(self, row):
        # Name2Id belongs to each shard; the SDK's resource-database lookup is both wrong and expensive here.
        kind = self._msg_type_name(row['local_type'])
        content = row['message_content']
        if isinstance(content, bytes):
            content = self._friendly_content(content, kind)
        if isinstance(content, str) and not content.strip():
            content = '[' + kind + ']'
        if content == '[' + kind + ']':
            compressed = row['compress_content']
            if isinstance(compressed, bytes) and compressed:
                decoded = self._friendly_content(compressed, kind)
                if decoded != content:
                    content = decoded
        message = {'local_id': row['local_id'], 'type': kind, 'sender_id': row['real_sender_id'],
            'sender_username': '', 'create_time': row['create_time'], 'content': content,
            'sort_seq': row['sort_seq'], 'shard': row['shard'], 'source': row['source']}
        if message['sender_id'] and message['sender_id'] != 2:
            # real_sender_id 指向所在消息分片的 Name2Id，不能查资源库的同号成员。
            rel = next((rel for rel, _, _ in self._db_files
                        if rel.replace(os.sep, '__') == message['shard']), None)
            if rel is None:
                raise RuntimeError('微信发送人所属分片不可读取')
            if rel not in self._message_senders:
                conn = self._open(rel)
                try:
                    self._message_senders[rel] = dict(conn.execute('SELECT rowid, user_name FROM Name2Id'))
                finally:
                    conn.close()
            message['sender_username'] = self._message_senders[rel].get(int(message['sender_id']), '')
        return message

    def get_new_message_page(self, group, position, limit=200):
        """A bounded page with a total cursor, including messages that share sort_seq."""
        limit = max(0, int(limit))
        sequence, shard, local_id = int(position['sort_seq']), position['shard'], int(position['local_id'])
        def read(tables):
            rows = []
            for conn, table in tables:
                label = self._shard_label(conn)
                if label == shard:
                    where, params = 'WHERE sort_seq>? OR (sort_seq=? AND local_id>?)', (sequence, sequence, local_id)
                elif label > shard:
                    where, params = 'WHERE sort_seq>=?', (sequence,)
                else:
                    where, params = 'WHERE sort_seq>?', (sequence,)
                rows.extend(self._shard_rows([(conn, table)], where, params,
                    order_ext=self._MSG_ORDER_ASC, per_shard_limit=limit))
            return rows
        rows = self._run_msg_query(group, read) or []
        rows.sort(key=lambda row: (row['sort_seq'], row['shard'], row['local_id']))
        return [self._msg_row_to_dict(row) for row in rows[:limit]]

    def get_display_name(self, group, username):
        conn = self._contact_conn()
        if conn is None:
            return username
        try:
            contact = conn.execute('SELECT remark,nick_name FROM contact WHERE username=? LIMIT 1', (username,)).fetchone()
            if contact and contact['remark']:
                return contact['remark']
            if group.endswith('@chatroom') and group not in self._group_names:
                room = conn.execute('SELECT ext_buffer FROM chat_room WHERE username=? LIMIT 1', (group,)).fetchone()
                names = {}
                try:
                    for field, member in protobuf_bytes(room['ext_buffer'] if room else b''):
                        if field != 1:
                            continue
                        fields = dict(protobuf_bytes(member))
                        ident = fields.get(1, b'').decode('utf-8')
                        name = fields.get(2, b'').decode('utf-8')
                        if ident and name:
                            if ident in names:
                                raise ValueError('微信群昵称成员重复')
                            names[ident] = name
                except (ValueError, UnicodeError):
                    names = {}
                self._group_names[group] = names
            return self._group_names.get(group, {}).get(username) or (contact['nick_name'] if contact else '') or username
        finally:
            conn.close()

    def get_avatar(self, username):
        rel = next((rel for rel, _, _ in self._db_files if Path(rel).name == 'head_image.db'), None)
        if not username or rel is None:
            return None
        conn = self._open(rel)
        try:
            row = conn.execute('SELECT image_buffer FROM head_image WHERE username=? AND length(image_buffer) BETWEEN 1 AND 2097152 LIMIT 1',
                               (username,)).fetchone()
            return bytes(row[0]) if row and isinstance(row[0], bytes) else None
        finally:
            conn.close()

    @staticmethod
    def signature(path):
        try:
            info = path.stat()
            return info.st_mtime_ns, info.st_size
        except FileNotFoundError:
            return None

    def _open_unlocked(self, rel):
        key = self._keys.get(rel)
        if key is None:
            raise RuntimeError('微信数据库密钥不可用，请重新连接当前登录的微信')
        source = Path(self._db_path(rel))
        wal_path = Path(str(source) + '-wal')
        for attempt in range(3):
            signature = self.signature(source), self.signature(wal_path)
            cached = self._snapshots.get(rel)
            if cached and cached[0] == signature:
                data = cached[1]
            else:
                encrypted = source.read_bytes()
                try:
                    wal = wal_path.read_bytes()
                except FileNotFoundError:
                    wal = b''
                if signature != (self.signature(source), self.signature(wal_path)):
                    continue
                if not encrypted or len(encrypted) % PAGE_SZ:
                    raise RuntimeError('微信数据库尚未写入完整页，未推进同步进度')
                data = bytearray(len(encrypted))
                for offset in range(0, len(encrypted), PAGE_SZ):
                    data[offset:offset + PAGE_SZ] = _decrypt_page(key, encrypted[offset:offset + PAGE_SZ], offset // PAGE_SZ + 1)
                frames, size = committed_frames(wal)
                if size is not None:
                    # 只接受已经提交的事务，不能把尾部未提交帧当作新消息。
                    if size * PAGE_SZ > len(encrypted) + len(wal):
                        raise RuntimeError('微信 WAL 数据库大小无效，未推进同步进度')
                    if len(data) < size * PAGE_SZ:
                        data.extend(b'\0' * (size * PAGE_SZ - len(data)))
                    for number, page in frames:
                        if number <= size:
                            data[(number - 1) * PAGE_SZ:number * PAGE_SZ] = _decrypt_page(key, page, number)
                    del data[size * PAGE_SZ:]
                    data[28:32] = struct.pack('>I', size)
                data[18:20] = b'\1\1'
                if signature != (self.signature(source), self.signature(wal_path)):
                    continue
            conn = sqlite3.connect(':memory:', factory=MemoryConnection)
            try:
                conn.deserialize(data)
                conn.execute('PRAGMA temp_store=MEMORY')
                conn.execute('PRAGMA query_only=ON')
                if not cached or cached[0] != signature:
                    if conn.execute('PRAGMA quick_check').fetchone()[0] != 'ok':
                        raise RuntimeError('微信内存数据库校验失败，未推进同步进度')
                    if getattr(self._scope, 'active', False):
                        self._snapshots[rel] = signature, data
                conn.row_factory = sqlite3.Row
                conn.text_factory = _sqlite_text_factory
                conn.shard = rel.replace(os.sep, '__')
                conn.owner = threading.get_ident()
                self._connections.add(conn)
                return conn
            except Exception:
                conn.close()
                if attempt == 2:
                    raise
                self._snapshots.pop(rel, None)
        raise RuntimeError('微信数据库正在更新，本轮未读取消息，请稍后重试')

    def _shard_label(self, conn):
        return conn.shard

    def reload_message(self, group, locator):
        rows = self._run_msg_query(group, lambda tables: self._shard_rows(
            [(conn, table) for conn, table in tables if self._shard_label(conn) == locator.get('shard', '')],
            'WHERE local_id=? AND sort_seq=?', (locator['local_id'], locator['sort_seq'])))
        if not rows or len(rows) != 1:
            raise RuntimeError('微信原消息已删除或无法唯一读取，未重新发送')
        return self._msg_row_to_dict(rows[0])

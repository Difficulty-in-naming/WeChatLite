"""Offline WeChat-style backward pages, never loads a native adapter."""
import json
from pathlib import Path
import sys

directory = Path(sys.argv[1])
groups = ['fixture-a', 'fixture-b']
held = []
selected = []

def emit(value):
    print(json.dumps(value), flush=True)

def source(group, index):
    return f'wx:{group}:message__message_0.db:{index}:{index}'

def message(group, index):
    # Several rows deliberately share a second on opposite sides of a page boundary.
    return {'id': source(group, index), 'source_id': source(group, index), 'group': group,
        'sender': 'Offline member', 'text': f'Offline row {index}', 'timestamp': 1700000000000 + (index // 7) * 1000,
        'status': 'received', 'media': []}

def page(command):
    if (directory / 'history-error').exists():
        emit({'type': 'history_done', 'request_id': command['request_id'], 'group': command['group'], 'error': 'Offline read failed'})
        return
    group = command['group']
    before = int(command['before_source'].rsplit(':', 1)[1]) if command['before_source'] else 138
    rows = list(range(min(137, before - 1), 0, -1))
    chunk = rows[:60]
    for index in reversed(chunk):
        emit({'type': 'message', 'historical': True, 'message': message(group, index)})
    emit({'type': 'history_done', 'request_id': command['request_id'], 'group': group,
        'before_source': source(group, chunk[-1]) if chunk else command['before_source'],
        'has_more': len(rows) > 60, 'sources': [source(group, index) for index in chunk]})

emit({'type': 'groups', 'groups': [{'id': group, 'name': 'Offline ' + group} for group in groups]})
emit({'type': 'status', 'ready': True, 'text': 'Offline history fixture'})
for line in sys.stdin:
    command = json.loads(line)
    with (directory / 'commands.jsonl').open('a', encoding='utf-8') as log:
        log.write(json.dumps(command) + '\n')
    if not (directory / 'hold-history').exists():
        while held:
            page(held.pop(0))
    if command['op'] == 'configure':
        selected = command['groups']
    elif command['op'] == 'activate':
        for index in range(98, 138):
            emit({'type': 'message', 'message': message(command['group'], index)})
    elif command['op'] == 'history':
        if (directory / 'hold-history').exists():
            held.append(command)
        else:
            page(command)

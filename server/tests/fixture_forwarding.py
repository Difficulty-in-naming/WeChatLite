"""Deliberately emits both groups to exercise relay authorization, never WeChat."""
import json
import os
from pathlib import Path
import sys
import time

directory = Path(sys.argv[1])
(directory / 'initial-policy.json').write_text(os.environ['WECHAT_LITE_FORWARD_GROUPS'], encoding='utf-8')
groups = ['fixture-a', 'fixture-b']
held = None
counter = 0


def emit(event):
    print(json.dumps(event), flush=True)


emit({'type': 'groups', 'groups': [{'id': group, 'name': 'Private catalog ' + group, 'count': 2} for group in groups]})
emit({'type': 'status', 'ready': True, 'text': 'Offline authorization fixture'})
for line in sys.stdin:
    command = json.loads(line)
    with (directory / 'commands.jsonl').open('a', encoding='utf-8') as log:
        log.write(json.dumps(command) + '\n')
    if command['op'] == 'configure':
        for group in command['groups']:
            emit({'type': 'members', 'group': group, 'members': [{'id': 'member-' + group, 'name': 'Member ' + group}]})
        if held and held['group'] not in command['groups']:
            emit({'type': 'ack', 'id': held['id'], 'status': 'sent', 'source_id': 'ack-' + held['id']})
            held = None
    elif command['op'] == 'activate':
        counter += 1
        for group in groups:
            source = directory / (group + '.dat')
            source.write_bytes(('private bytes ' + group).encode())
            ident = f'{group}-{counter}'
            emit({'type': 'message', 'message': {'id': ident, 'source_id': ident, 'group': group,
                'sender': 'Member ' + group, 'text': 'Private body ' + group, 'timestamp': int(time.time() * 1000),
                'status': 'received', 'media': [{'id': 'native-' + ident, 'name': group + '.dat', 'path': str(source),
                    'kind': 'file', 'size': source.stat().st_size, 'sha256': ''}]}})
    elif command['op'] == 'send':
        message = command['message']
        if message['text'].startswith('hold'):
            held = message
        else:
            emit({'type': 'ack', 'id': message['id'], 'status': 'sent', 'source_id': 'ack-' + message['id'],
                'source_ids': ['ack-' + message['id'], 'second-part-' + message['id']]})
    elif command['op'] in ('recall', 'recall_check'):
        message = command['message']
        if command['op'] == 'recall' and message['text'].startswith('recall hold'):
            continue
        status = 'failed' if message['text'].startswith('recall fail') else 'uncertain' if message['text'].startswith('recall uncertain') else 'recalled'
        emit({'type': 'recall_ack', 'id': message['id'], 'status': status, 'error': '' if status == 'recalled' else 'Fixture confirmation unavailable'})

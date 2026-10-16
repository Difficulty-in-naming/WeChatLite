"""Measure this app's owned process tree. No messages are sent to WeChat."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import psutil

parser = argparse.ArgumentParser()
parser.add_argument('executable')
parser.add_argument('--seconds', type=int, default=20)
parser.add_argument('--output', default='artifacts/performance.json')
args = parser.parse_args()
with tempfile.TemporaryDirectory() as profile:
    env = dict(os.environ, WECHAT_LITE_PROFILE=profile)
    command = [str(Path(args.executable).resolve()), '--demo']
    process = subprocess.Popen(command, env=env, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    owner = psutil.Process(process.pid)
    samples, known, previous = [], {}, {}
    try:
        time.sleep(7)
        for _ in range(args.seconds):
            current = [owner] + owner.children(recursive=True)
            sample = []
            for item in current:
                try:
                    cpu = item.cpu_times()
                    now = time.monotonic()
                    used = cpu.user + cpu.system
                    old = previous.get(item.pid)
                    sample.append({'pid': item.pid, 'name': item.name(), 'rss_mb': round(item.memory_info().rss / 1024**2, 2),
                        'private_mb': round(getattr(item.memory_info(), 'private', item.memory_info().rss) / 1024**2, 2),
                        'single_core_cpu_percent': round((used-old[1])/(now-old[0])*100, 3) if old else 0})
                    previous[item.pid] = (now, used)
                    known[item.pid] = item
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass
            samples.append(sample)
            time.sleep(1)
    finally:
        for item in reversed(list(known.values())):
            try:
                item.terminate()
            except psutil.NoSuchProcess:
                pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
    by_process = {}
    for sample in samples[1:]:
        for item in sample:
            by_process.setdefault(item['name'], []).append(item)
    summary = {name: {'average_rss_mb': round(sum(i['rss_mb'] for i in entries)/len(entries),2),
        'average_private_mb': round(sum(i['private_mb'] for i in entries)/len(entries),2),
        'average_single_core_cpu_percent': round(sum(i['single_core_cpu_percent'] for i in entries)/len(entries),3)} for name,entries in by_process.items()}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({'scenario': 'idle demo client and relay; no real WeChat sends', 'seconds': args.seconds, 'summary': summary, 'samples': samples}, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False, indent=2))

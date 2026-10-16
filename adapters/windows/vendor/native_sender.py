"""Load the verified native sender into the existing WeChat main process.

No window, keyboard, mouse or clipboard APIs. No install-directory changes.
"""
import ctypes
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import re
import secrets
import shutil
import socket
import time
import urllib.error
import urllib.request

import psutil

CLIENT_HASH = '10f8e995453e2da46d4f2b5080cd6da1f13cc5147746adc119ceae38cb039de5'
VERSION = '4.1.15.13'


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', ctypes.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', W.WCHAR * 256),
                ('szExePath', W.WCHAR * 260)]


def kernel_api():
    dll = ctypes.WinDLL('kernel32', use_last_error=True)
    signatures = {
        'CreateToolhelp32Snapshot': (W.HANDLE, [W.DWORD, W.DWORD]),
        'Module32FirstW': (W.BOOL, [W.HANDLE, ctypes.POINTER(MODULEENTRY32W)]),
        'Module32NextW': (W.BOOL, [W.HANDLE, ctypes.POINTER(MODULEENTRY32W)]),
        'CloseHandle': (W.BOOL, [W.HANDLE]),
        'OpenProcess': (W.HANDLE, [W.DWORD, W.BOOL, W.DWORD]),
        'VirtualAllocEx': (ctypes.c_void_p, [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD, W.DWORD]),
        'VirtualFreeEx': (W.BOOL, [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD]),
        'WriteProcessMemory': (W.BOOL, [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]),
        'GetModuleHandleW': (W.HMODULE, [W.LPCWSTR]),
        'GetModuleHandleExW': (W.BOOL, [W.DWORD, ctypes.c_void_p, ctypes.POINTER(W.HMODULE)]),
        'GetModuleFileNameW': (W.DWORD, [W.HMODULE, W.LPWSTR, W.DWORD]),
        'GetProcAddress': (ctypes.c_void_p, [W.HMODULE, ctypes.c_char_p]),
        'CreateRemoteThread': (W.HANDLE, [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD)]),
        'WaitForSingleObject': (W.DWORD, [W.HANDLE, W.DWORD]),
    }
    for name, (result, args) in signatures.items():
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = result, args
    return dll


def modules(pid):
    kernel = kernel_api()
    handle = kernel.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    entry = MODULEENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    result = {}
    try:
        more = kernel.Module32FirstW(handle, ctypes.byref(entry))
        while more:
            result[entry.szModule.lower()] = (int(entry.modBaseAddr), Path(entry.szExePath))
            more = kernel.Module32NextW(handle, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(handle)
    if not result:
        raise RuntimeError('无法读取微信主进程的模块列表')
    return result


def main_process():
    found = []
    for process in psutil.process_iter(['pid', 'name', 'cmdline', 'exe']):
        info = process.info
        if (info['name'] or '').lower() != 'weixin.exe':
            continue
        if any(arg.startswith('--type=') or arg == '--crashpad-handler' for arg in info['cmdline'] or []):
            continue
        loaded = modules(info['pid'])
        if 'weixin.dll' in loaded:
            found.append((info['pid'], loaded))
    if len(found) != 1:
        raise RuntimeError('需要恰好一个已登录的微信主进程，未加载发送组件')
    pid, loaded = found[0]
    path = loaded['weixin.dll'][1]
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if path.parent.name != VERSION or digest != CLIENT_HASH:
        raise RuntimeError('微信版本或 DLL 指纹不匹配，只适配未经修改的 4.1.15.13')
    return pid, loaded


def load_library(pid, loaded, path):
    if any(candidate.resolve() == path.resolve() for _, candidate in loaded.values()):
        return
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise RuntimeError('本地发送组件要求 64 位 Python/程序')
    kernel = kernel_api()
    address = kernel.GetProcAddress(kernel.GetModuleHandleW('kernel32.dll'), b'LoadLibraryW')
    containing = W.HMODULE()
    if not address or not kernel.GetModuleHandleExW(0x4 | 0x2, address, ctypes.byref(containing)):
        raise ctypes.WinError(ctypes.get_last_error())
    name = ctypes.create_unicode_buffer(260)
    kernel.GetModuleFileNameW(containing, name, 260)
    remote_base = loaded[Path(name.value).name.lower()][0]
    start = remote_base + address - containing.value
    process = kernel.OpenProcess(0x2 | 0x8 | 0x10 | 0x20 | 0x400, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    allocation = thread = None
    completed = False
    try:
        data = ctypes.create_unicode_buffer(str(path.resolve()))
        allocation = kernel.VirtualAllocEx(process, None, ctypes.sizeof(data), 0x3000, 0x4)
        written = ctypes.c_size_t()
        if not allocation or not kernel.WriteProcessMemory(process, allocation, data, ctypes.sizeof(data), ctypes.byref(written)):
            raise ctypes.WinError(ctypes.get_last_error())
        if written.value != ctypes.sizeof(data):
            raise RuntimeError('组件路径写入不完整，未执行加载')
        thread = kernel.CreateRemoteThread(process, None, 0, start, allocation, 0, None)
        if not thread:
            raise ctypes.WinError(ctypes.get_last_error())
        if kernel.WaitForSingleObject(thread, 30000) != 0:
            raise RuntimeError('微信组件加载等待超时，未发送任何消息')
        completed = True
    finally:
        if allocation and (completed or not thread):
            kernel.VirtualFreeEx(process, allocation, 0, 0x8000)
        if thread:
            kernel.CloseHandle(thread)
        kernel.CloseHandle(process)
    if not any(candidate.resolve() == path.resolve() for _, candidate in modules(pid).values()):
        raise RuntimeError('微信未成功加载本地发送 DLL')


class NativeClient:
    def __init__(self, directory, dll_path):
        self.root, self.dll_path = Path(directory), Path(dll_path)
        self.directory = self.root
        self.pid = None
        self.token = ''
        self.port = 30001

    def request(self, path, body=None):
        data = json.dumps(body, ensure_ascii=False).encode('utf-8') if body is not None else None
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}' + path, data=data,
            headers={'Authorization': 'Bearer ' + self.token, 'Content-Type': 'application/json'})
        # A local service must never be sent through an environment-configured proxy.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=20) as response:
            return json.load(response)

    def connect(self, groups, media_root=None, *, disable_older=True):
        groups = sorted(set(groups))
        if not groups or any(not re.fullmatch(r'[0-9]{5,24}@chatroom', group) for group in groups):
            raise ValueError('需要明确选择可同步的微信群')
        self.pid, loaded = main_process()
        if not self.dll_path.is_file():
            raise RuntimeError('缺少已编译的本地发送 DLL')
        digest = hashlib.sha256(self.dll_path.read_bytes()).hexdigest()
        self.directory = self.root / digest[:16]
        self.directory.mkdir(parents=True, exist_ok=True)
        destination = self.directory / ('WeChatLocal_' + digest[:16] + '.dll')
        config = self.directory / 'native.json'
        previous = json.loads(config.read_text('utf-8')) if config.exists() else {}
        self.token = previous.get('token', '')
        if not re.fullmatch(r'[a-f0-9]{64}', self.token):
            self.token = secrets.token_hex(32)
        already_loaded = any(candidate.resolve() == destination.resolve() for _, candidate in loaded.values())
        if not already_loaded:
            shutil.copyfile(self.dll_path, destination)
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                self.port = sock.getsockname()[1]
        else:
            self.port = int(previous['port'])
        media_root = Path(media_root or self.root.parent / 'feishu/media').resolve()
        media_root.mkdir(parents=True, exist_ok=True)
        temporary = config.with_suffix('.tmp')
        temporary.write_text(json.dumps({'token': self.token, 'groups': groups,
            'port': self.port, 'media_root': str(media_root)}), 'utf-8')
        temporary.replace(config)
        load_library(self.pid, loaded, destination)
        deadline = time.monotonic() + 20
        while True:
            try:
                health = self.request('/health')
                if health.get('pid') != self.pid or health.get('version') != VERSION or health.get('error'):
                    raise RuntimeError('本地发送组件的进程或版本校验未通过')
                # Loaded modules cannot be unloaded while native callbacks may use them.
                # Disable their allowlists; normal WeChat exit releases the old modules.
                for old in ([self.root / 'native.json', *self.root.glob('*/native.json')] if disable_older else []):
                    if old == config or not old.is_file():
                        continue
                    value = json.loads(old.read_text('utf-8'))
                    value['groups'] = []
                    temp = old.with_suffix('.tmp')
                    temp.write_text(json.dumps(value), 'utf-8')
                    temp.replace(old)
                return health
            except urllib.error.URLError:
                if time.monotonic() >= deadline:
                    raise RuntimeError('本地发送组件未就绪；消息未发送') from None
                time.sleep(.2)

    def send(self, group, text, key, atlist=''):
        if not self.pid or not psutil.pid_exists(self.pid):
            raise RuntimeError('微信主进程已退出，请重新开始同步以连接后台发送')
        request_id = hashlib.sha256(key.encode()).hexdigest()
        return self.request('/SendTextMsg', {'wxidorgid': group, 'msg': text, 'atlist': atlist, 'request_id': request_id})

    def send_media(self, group, path, kind, key):
        if not self.pid or not psutil.pid_exists(self.pid):
            raise RuntimeError('微信主进程已退出，请重新开始同步以连接后台发送')
        request_id = hashlib.sha256(key.encode()).hexdigest()
        return self.request('/SendMediaMsg', {'wxidorgid': group, 'path': str(Path(path).resolve()),
            'kind': kind, 'request_id': request_id})

    def download_video(self, group, file_id, aes_key, length, key):
        if not self.pid or not psutil.pid_exists(self.pid):
            raise RuntimeError('微信主进程已退出，请重新开始同步以连接后台下载')
        return self.request('/DownloadVideo', {'group': group, 'file_id': file_id, 'aes_key': aes_key,
            'length': length, 'request_id': hashlib.sha256(key.encode()).hexdigest()})

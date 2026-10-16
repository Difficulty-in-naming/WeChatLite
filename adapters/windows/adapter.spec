# Run from the project root: .tools/Scripts/pyinstaller.exe --clean --noconfirm adapters/windows/adapter.spec
from pathlib import Path
from importlib.metadata import distribution
from PyInstaller.utils.hooks import copy_metadata

root = Path(SPECPATH)
package = Path(distribution('wechatauto-replica').locate_file('wechatauto'))
needed = {'__init__', 'db', 'media', 'param'}
unused = ['wechatauto.' + path.stem for path in package.glob('*.py') if path.stem not in needed]
unused += ['wechatauto.utils', 'wechatauto.msgs']
pilk_license = Path(distribution('pilk').locate_file('pilk-0.2.4.dist-info/LICENSE'))
datas = copy_metadata('wechatauto-replica') + copy_metadata('pilk')
datas += [(str(root / 'vendor/native/WeChatLocal.dll'), 'vendor/native'),
          (str(root / 'vendor/WechatAuto-LICENSE.txt'), 'licenses'),
          (str(pilk_license), 'licenses/pilk')]

analysis = Analysis([str(root / 'bridge_main.py')], pathex=[str(root), str(root / 'vendor')],
    binaries=[], datas=datas,
    hiddenimports=['wechatauto.db', 'wechatauto.media', 'wechatauto.param', 'memory_db', 'bridge',
        'native_sender', 'wxgf', 'pilk', 'pilk._pilk', 'zstandard', 'imageio_ffmpeg', 'PIL.Image',
        'PIL.GifImagePlugin', 'PIL.JpegImagePlugin', 'PIL.PngImagePlugin'],
    hookspath=[], hooksconfig={}, runtime_hooks=[],
    excludes=unused + ['cv2', 'numpy', 'uiautomation', 'pyautogui', 'PySide6', 'PyQt6', 'PyQt5',
        'tkinter', 'matplotlib', 'pytest', 'win32com', 'pythoncom'], noarchive=False)
pyz = PYZ(analysis.pure)
exe = EXE(pyz, analysis.scripts, [], exclude_binaries=True, name='wechat-adapter',
    debug=False, bootloader_ignore_signals=False, strip=False, upx=False, console=True)
coll = COLLECT(exe, analysis.binaries, analysis.datas, strip=False, upx=False, name='wechat-adapter')

"""将微信 WXGF 的独立颜色、遮罩码流还原为图片，不读取或操作微信进程。"""
import io
import re
import subprocess

from PIL import Image

from bridge import Unavailable


def _nal_units(stream):
    starts = list(re.finditer(b'\x00\x00\x00\x01|\x00\x00\x01', stream))
    for index, start in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(stream)
        unit = stream[start.end():end]
        if len(unit) >= 2:
            yield (unit[0] >> 1) & 63, unit


def _chroma(stream):
    sps = next((unit for kind, unit in _nal_units(stream) if kind == 33), None)
    if sps is None:
        raise ValueError('缺少 HEVC 参数')
    rbsp = re.sub(b'\x00\x00\x03', b'\x00\x00', sps[2:])
    bits = ''.join(f'{byte:08b}' for byte in rbsp)
    position = 0

    def take(count):
        nonlocal position
        if position + count > len(bits):
            raise ValueError('HEVC 参数截断')
        value = int(bits[position:position + count], 2) if count else 0
        position += count
        return value

    def ue():
        zeros = 0
        while take(1) == 0:
            zeros += 1
            if zeros > 31:
                raise ValueError('HEVC 参数无效')
        return (1 << zeros) - 1 + take(zeros)

    take(4)
    layers = take(3)
    take(1 + 96)
    flags = [(take(1), take(1)) for _ in range(layers)]
    if layers:
        take(2 * (8 - layers))
    for profile, level in flags:
        take(88 if profile else 0)
        take(8 if level else 0)
    ue()
    chroma = ue()
    if chroma not in (0, 1, 2, 3):
        raise ValueError('HEVC 颜色格式无效')
    return chroma


def _partitions(data):
    start = re.search(b'(?:\x00\x00\x00\x01|\x00\x00\x01)\x40\x01', data)
    if start is None or start.start() < 4:
        raise ValueError('缺少 WXGF 图像码流')
    position, parts = start.start() - 4, []
    while position < len(data):
        size = int.from_bytes(data[position:position + 4], 'big')
        position += 4
        if not size or position + size > len(data) or len(parts) >= 2:
            raise ValueError('WXGF 分片不完整或尚未支持')
        stream = data[position:position + size]
        # 静态图片的遮罩和颜色是两路码流，不能当作同一路视频的前两帧。
        if sum(kind < 32 and len(unit) > 2 and bool(unit[2] & 128)
               for kind, unit in _nal_units(stream)) != 1:
            raise ValueError('WXGF 多帧图像尚未支持')
        parts.append((_chroma(stream), stream))
        position += size
    return parts


def decode_wxgf(data):
    """只返回完整 PNG；未知或多帧格式保留待处理，绝不拿遮罩当正文图片。"""
    import imageio_ffmpeg
    if not isinstance(data, bytes) or not data.startswith(b'wxgf') or len(data) > 10 * 1024 * 1024:
        raise Unavailable('微信 WXGF 图片无效或过大，未发送')
    try:
        parts = _partitions(data)
        if len(parts) == 2:
            color = [stream for chroma, stream in parts if chroma != 0]
            masks = [stream for chroma, stream in parts if chroma == 0]
            if len(color) != 1 or len(masks) != 1:
                raise ValueError('无法区分 WXGF 颜色和遮罩')
        else:
            color, masks = [parts[0][1]], []

        def frame(stream):
            result = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-xerror',
                '-threads', '1', '-f', 'hevc', '-i', 'pipe:0', '-frames:v', '1',
                '-f', 'image2pipe', '-vcodec', 'png', 'pipe:1'], input=stream,
                capture_output=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if result.returncode or not result.stdout or len(result.stdout) > 10 * 1024 * 1024:
                raise ValueError('WXGF 图像无法完整解码')
            with Image.open(io.BytesIO(result.stdout)) as picture:
                picture.load()
                return picture.copy()

        picture = frame(color[0]).convert('RGB')
        if masks:
            mask = frame(masks[0]).convert('L')
            if mask.size != picture.size:
                raise ValueError('WXGF 颜色和遮罩尺寸不一致')
            if mask.getextrema() != (255, 255):
                picture.putalpha(mask)
        output = io.BytesIO()
        picture.save(output, format='PNG')
        return output.getvalue()
    except (ValueError, OSError, subprocess.TimeoutExpired, Image.DecompressionBombError) as exc:
        raise Unavailable('微信 WXGF 图片尚未完整解码，原消息已保留') from exc

# Third-party components

The client is C++/Qt with an optional Rust P2P transport helper. The relay and Windows bridge logic are Rust, including
database snapshots, media handling, confirmation and durable receipt state.
The release runtime does not contain Python or PyInstaller. It uses the native
components below. The adapter runtime directory contains `wechat-adapter.exe`,
`WeChatActions.dll`, `WeChatQuote.dll` and `ffmpeg.exe`. No account credentials, decrypted databases or chat history
are included in distributable packages.

| Component | License and retained source / notices |
| --- | --- |
| Qt 6.8.3 dynamic Widgets, Network and WebSockets libraries | Module and bundled-code terms vary; [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [matching source archive](https://download.qt.io/archive/qt/6.8/6.8.3/). The application links Qt dynamically. |
| Rust relay/adapter dependencies | Exact versions/checksums are in `Cargo.lock`; each crate retains its license. This includes SQLite, image codecs and cryptography/networking dependencies. |
| iroh / iroh-relay 1.3.0 and noq QUIC transport | MIT OR Apache-2.0, with retained BSD-3-Clause portions; [upstream](https://github.com/n0-computer/iroh/tree/v1.3.0). Matching notices are in `network/licenses` and package `licenses/`. Used by the Rust relay and `wechat-link` client helper; the separately deployed public relay is the pinned official Linux release. Dependencies and source checksums are retained in `Cargo.lock`. |
| Local database/media algorithms ported from wechatauto-replica 1.2.6 | Apache-2.0, [upstream source](https://github.com/fanyuantaier/wechatauto-replica); original notice in `adapters/windows/vendor/WechatAuto-LICENSE.txt` and package `licenses/WechatAuto-Apache-2.0.txt`. |
| Original version-verified WeChatLocal sender | User-supplied local native component in `adapters/windows/vendor/native/WeChatLocal.dll`, retained from the supplied WeChatFeishuBridge project. It is embedded by the Rust adapter; the historical Python bridge is not executed. |
| WeChatActions recall / WeChatQuote native text-quote modules | Reviewable project source and standalone build with two offline self-test executables in `adapters/native`; third-party MIT/BSD notices are retained there and copied to package `licenses/`. WeChatQuote is a separate, hook-free module; it does not replace the existing recall DLL. |
| MinHook | Original license in `adapters/native/third_party/minhook/LICENSE.txt`, copied to package `licenses/MinHook.txt`. |
| cpp-httplib 0.12.6 / nlohmann JSON 3.11.1 | MIT; unmodified headers and full attribution/license text in `adapters/native/include` and `adapters/native/THIRD_PARTY_NOTICES.md`. |
| SILK C SDK from pilk 0.2.4 | The SDK retains Skype Limited's 2006–2012 redistribution terms in each C/header file. The enclosing pilk source distribution retains GPL-3.0. Both are preserved in `adapters/windows-rs/vendor/pilk-0.2.4`; the complete original archive is `thirdparty-sources/pilk-0.2.4.tar.gz`. Only SDK C sources are compiled; no Python extension is linked. |
| Standalone FFmpeg 7.1 CLI, default Windows artifact | GPL-3.0-or-later, as reported by its `-L`; see the exact artifact and source details below. It is invoked only when needed for WXGF, independently of Qt's FFmpeg DLLs. |

## Standalone FFmpeg artifact and source

The reused EXE identifies itself as
`7.1-essentials_build-www.gyan.dev`, built with GCC 14.2.0 from MSYS2.
Its configure line includes `--enable-gpl --enable-version3 --enable-static`,
`--enable-libx264` and `--enable-libx265`; the complete actual flags and
library version output are retained in
`thirdparty-sources/ffmpeg-7.1-buildinfo.txt`. The release build also records the
selected EXE's own `-version` and `-L` output in `licenses/ffmpeg-build.txt`.
The full GPL version 3 text is supplied as
`thirdparty-sources/FFmpeg-COPYING.GPLv3` and package
`licenses/FFmpeg-COPYING.GPLv3`.

This EXE was reused from the already installed imageio-ffmpeg binary bundle;
its Python wrapper is not copied or called. The wrapper distribution's separate
BSD notice is retained as provenance when available. That BSD license is not a
replacement for the EXE's GPL terms. The [Gyan build description](https://www.gyan.dev/ffmpeg/builds/)
identifies these static builds as GPLv3; the
[7.1 release](https://github.com/GyanD/codexffmpeg/releases/tag/7.1) records the
FFmpeg source revision.

The unmodified FFmpeg upstream 7.1 source is included in
`thirdparty-sources/ffmpeg-7.1.tar.xz`, downloaded directly from the
[official release archive](https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz).
Its SHA256 is
`40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6`.
The archive is copied into the Windows package's `thirdparty-sources/`.

That tarball contains FFmpeg itself. It is not described as the complete
corresponding source of every external library statically linked into Gyan's
EXE. Source/version references for the build's external dependencies are
available through the [Gyan release and library list](https://www.gyan.dev/ffmpeg/builds/#libraries)
and FFmpeg's [external-library documentation](https://ffmpeg.org/general.html#External-libraries).
For a rebuild, retain the exact release, GCC/MSYS2 toolchain and configure flags,
obtain the matching external-library sources and apply their own build options;
the [FFmpeg Windows build guide](https://ffmpeg.org/platform.html#Windows)
describes the MinGW/MSYS2 environment. The saved flags alone are not a claim of
byte-identical reproduction.

The current artifact is for local personal use. These files record the sources
and license material present here; they do not assert that the upstream tarball
alone satisfies every condition for arbitrary external redistribution. Any
replacement supplied with `-FFmpeg` needs its own matching notices and source
inventory. FFmpeg's [license documentation](https://ffmpeg.org/legal.html)
describes how enabled components affect its terms.

## Historical source

`adapters/windows`, its requirements/spec and Python regression tests remain as
migration history. Python 3.11 (PSF), PyInstaller (GPL-2.0 with its bootloader
exception), pilk's Python wrapper and the old OpenCV/UI-automation dependency
chain are not part of the new runtime package. Previous packaged binaries are
preserved under `build/legacy-python-adapter/` when the release script migrates
them; they are not copied into the new adapter directory.

The application is independent of Tencent and WeChat. Version checks describe
the supplied local adapter rather than an official cross-platform WeChat API.

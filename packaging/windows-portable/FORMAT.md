The portable EXE is a small Windows GUI-subsystem launcher built with the static
MSVC C runtime, followed by a WLPKG01 payload and a 24-byte footer. Qt and the
application's other dependencies remain dynamic files in a persistent per-user
cache. This is not a statically linked Qt application.

The payload consists of `WLPKG01\n`, a little-endian u64 JSON-manifest length,
the UTF-8 manifest, then each regular file's bytes in manifest order. The footer
is `WLPEND1\n`, a little-endian u64 payload offset and a u64 payload length.
The build script compiles the independent complete-payload SHA256 into the stub.
The payload is never read into one allocation: hashing/copying uses 64 KiB
buffers; the manifest is limited to 4 MiB. There is no ZIP extractor or link type.

Only runtime files explicitly allowed by `allowed_file` enter the package.
Licenses and the server's required third-party source archives are retained.
Configuration, credentials, private keys and development files are rejected.
Paths reject traversal, drive/UNC/ADS syntax, reserved Windows device names,
duplicates and file/directory collisions. Source and cache symlinks, junctions
and hard-linked files are rejected.

The launcher obtains LocalAppData from SHGetKnownFolderPath, not an environment
override. Its own `WeChatLitePortable/<role>/<payload-sha256>` directory has a
protected DACL for the current user SID and LocalSystem. An OS file lock covers
validation and extraction. A fresh private staging directory is published by
rename only after every expected file is durable and verified. Later launches
verify the complete embedded payload and stream-verify the cache. Unexpected or
damaged caches are preserved under a new name; old versions are never deleted.

The actual GUI starts with the original arguments and caller working directory.
The launcher immediately exits, so Qt's existing QSettings/AppLocalDataLocation
names and configuration behavior remain in effect. Persistent cache paths also
remain usable by OS notification shortcuts registered by the actual GUI.

Build checked distribution directories with `scripts/package-portable-windows.ps1`.
This is an independent Cargo workspace with its own lockfile. Run its Cargo tests
for extraction/security fixtures and `test-launcher.ps1 -Offline` for an actual
static-CRT launcher carrying a harmless argument-reporting fixture. The fixture
does not start the application or connect to WeChat. Test caches are deliberately
retained, like production caches. Sign the final EXE after appending the payload;
modifying an already signed EXE invalidates Authenticode.

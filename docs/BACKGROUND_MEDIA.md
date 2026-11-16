# Background media downloads (Windows WeChat 4.1.15.13)

On adapter reconnect, the relay queues existing legacy record summaries by their exact native source, including cards outside the 40-message warm window. The queue is bounded per loop and follows current group selection. Successfully reread attachments can also repair an older card that was incorrectly marked complete with unavailable media. Existing message IDs and sequence positions remain stable.

Merged-forward cards are also decoded from `recorditem/recordinfo/datalist`. The screenshot's real three-message record yielded two complete images of 52,342 and 5,006 bytes, verified against `datasize/fullmd5`; neither thumbnail resource was used. The client displays a clickable card and ordered, read-only details. Text remains visible while attachments download and the relay withholds its receipt until they finish. All attachment IDs remain owned by the outer group. Both client and server must be updated for the detail view. Creating or sending a new merged-forward card is outside this receiving change.

The download allowlist now follows all administrator-selected groups independently of native sending. Unopened groups can download without activating either chat UI. Removing a group or shutting down revokes its download grant. Unfinished record attachments use the normal short media retry schedule; history pages durably queue cards beyond their immediate conversion budget so they cannot be skipped by pagination.

The previously verified 843,587-byte standard video is H.264, 720×1280, 30 fps, 13.44 seconds, with AAC audio. It is a complete compressed/transcoded playback copy, not proof of the sender's original quality. No WeChat setting is needed to raise the old native endpoint's 30 MiB application limit; the new component accepts up to 512 MiB. The 512 MiB boundary was checked offline, while the real large-file sample was 179,873,730 bytes.

The previous adapter read image and attachment caches but never requested their download. Opening the original chat caused WeChat to populate those caches, which made forwarding appear to depend on the foreground window. The legacy video endpoint additionally imposed a 30 MiB limit while the adapter advertised 512 MiB.

The new `WeChatMedia` module calls WeChat's existing CDN task API. It uses WeChat's real constructor/destructor, allocator and live callback owner; the adapter checks the exact client and component hashes. It writes private partial files without opening chats or activating windows. Modern `7f0c` file IDs require BigFile type 7; treating them as ordinary type-5 attachments never completed in the observed failures.

Video has a separate failure mode. A real message declared 520,420 bytes, but a successful native CDN download returned a complete 843,587-byte video with a different MD5. The native completion callback supplied the actual size and an MD5 matching the downloaded bytes. Its original-video request returned error -5103017; that code is retained as an error, without assuming a specific undocumented meaning. Relaxing XML checks or treating a stable partial-file size as completion would be incorrect.

The adapter therefore prefers an original video when available and also permits the standard video to complete independently. Only a successful same-request native receipt can authenticate a transcoded standard video's different size/hash. Cache receipts are bound to the group, source locator, media kind, resource ID, AES parameters and declared metadata through a SHA-256 request ID. Every subsequent read rechecks actual bytes against the receipt. Software/file attachments require their declared size and MD5. Ordinary and merged-forward pictures can be reencoded by the CDN: only a successful same-request completion receipt, actual size and matching CDN MD5 permit those replacement bytes. The picture must also fully decode. Cached picture receipts retain the exact native response and are revalidated against actual bytes on every read; an unverified partial file cannot authorize a size/hash difference. Merged-forward images do not use thumbnail URLs or type 2. The six-message sample produced two complete PNG images of 1535×974 and 2540×1199 pixels (72,907 and 76,976 bytes).

Ordinary image messages first try the local cache. With no usable local image, download metadata prefers the complete `cdnbigimgurl` / `hdlength` pair (native type 1), then the complete `cdnmidimgurl` / `length` pair (native type 2, `image_mid`). Each pair requires the message's valid `aeskey` and `md5`; fields are not mixed between the two resources. `cdnthumburl` is never substituted for a full-picture resource. The previous parser accepted only original-image fields and silently returned the local-cache error for standard-only messages. The updated warning describes field validity without printing XML, resource IDs, MD5 values or keys.

On 2026-11-16 the user supplied logs from the updated test server: native downloading began, then a completed picture was rejected with `微信 CDN 下载内容与原消息不一致`, and the adapter delivered a gray placeholder. This establishes a concrete failure in the completed-download handling; it does not by itself establish every remote production failure. Ordinary pictures now use the same receipt-backed reencoding validation as record pictures, including `image_mid`. Failed download or decode paths no longer emit a gray placeholder or acknowledge completion. The relay identifies previously delivered legacy blank images by their exact 691-byte PNG SHA-256 and queues their source for read-only adapter conversion, at most eight per loop in selected groups. Recovered previews/full pictures update the same message; client-owned or recalled messages remain protected.

The subsequent `cdn-receipt-v2` logs confirm that both new pictures and old blank-image refreshes remain pending at `图片缓存尚未完整写入`; receipt handling alone did not resolve reception. Code review found that local decryption strips short JPEG/PNG footers, while the CDN path skipped this step and required the end marker at the physical end of the file. `cdn-footer-v3` shares this existing normalization with ordinary and record CDN pictures: it first verifies the untouched download bytes, strips at most 32 trailing bytes after a JPEG/PNG end marker in the delivery copy, and still requires a full bounded decode. The raw download and receipt are unchanged. Missing end markers, larger tails, corrupt image data, and invalid receipts remain rejected. New failure diagnostics report only format, byte counts and end structure. The user's subsequent v3 logs report two JPEG downloads of 151,606 and 763,539 bytes, each normalized by removing 24 footer bytes and successfully converted. The user confirmed receipt in the client for both foreground and background scenarios in the local Windows VM. This establishes the cause for those pictures; remote production confirmation is separate.

`media-wait=delayed-clear-v1` keeps the initial `MediaPending` wait in server logs, then emits one warning after 30 seconds of continuous waiting, including when a thumbnail or partial record is already visible. The existing persisted wait timestamp survives adapter restarts. Warnings carry an `inbound_media` code, group and native source ID. The updated client clears its matching warning only when the corresponding complete received message is imported; another source, another group, a thumbnail, an incomplete record or an unrelated replacement warning cannot clear it. Pending work, retry intervals and completion receipts remain intact, and non-pending errors still warn. Both server and client require updating for this warning behavior.

`inbox-poll=snapshot-v1` fixes a separate polling cache ordering defect. A database write between cycles could change the version returned by `Db::signatures()` while `Db::page()` still queried a warm snapshot of the old version. The engine then marked the new version as scanned; end-of-cycle invalidation discarded the connection too late, so another write was needed to schedule a new scan. Signature collection now invalidates each message snapshot against the exact version it returns, while retaining unchanged shards. An encrypted database/WAL fixture reproduces the missing last image before the fix and reads it on the first scan afterward. This is code verification, not confirmation of the user's intermittent live delay. Local `at_ms` diagnostics separate first discovery from subsequent media processing and report slow snapshot, signature and cycle work without logging bodies or resource credentials.

Windows receipt validation also accepts an existing cache file's short and long path aliases. CI temporary roots can use an 8.3 username such as `RUNNER~1`, while `owned_dir` returns the expanded canonical directory. Comparing only their text rejected valid cached receipts and led two offline picture fixtures to the download authorization guard. The original failures were reproduced with a Windows short temporary path. When the textual paths differ, both must resolve to the exact same file within the private download directory; another file, even with identical bytes, remains rejected. Request, size, MD5 and decode checks remain required. This correction does not authorize new downloads.

If the full picture is pending or unusable, the adapter now tries the same message's local `<md5>_t.dat` and `<md5>.dat` as previews, including thumbnails smaller than 512 bytes. The bytes must decrypt and fully decode before delivery; unrelated files and partial images are rejected. A preview uses a separate `preview.*` cache and sets `media_pending`, so it cannot hide a later `image.*` result. The relay displays the preview but withholds the completion receipt, and the adapter keeps its bounded retry schedule across restarts. A verified full picture updates the original message's media while preserving its ID and timeline position. Duplicate previews do not create updates; late previews cannot downgrade a completed picture. Both the adapter and relay must be updated. No client update or native DLL change is needed for this local-thumbnail fallback. If no decodable local thumbnail exists, the adapter still waits for real picture data; this change does not add a CDN thumbnail endpoint.

Picture cache lookup prefers the explicit `img@md5` attribute. A CDN resource ID or AES value may appear earlier in XML and must not be treated as the picture's filename by taking its first 32 hexadecimal characters. Packed metadata remains a fallback when no explicit picture MD5 is available.

The native listener is loopback-only with a random bearer token, selected-group checks on every request, bounded request/receipt counts, stable request fingerprints and private hash-derived destinations. Stop/deselection prevents new downloads. Completion observation is limited to already-registered request IDs and delegates unchanged to WeChat's original callback. No new chat messages were sent during diagnosis or download verification. Frida was used only as temporary developer instrumentation to confirm the result layout; it was detached and is absent from the release runtime.

## Verification

User terminology: “local testing” means the user's hands-on test with real WeChat in a local Windows VM. On 2026-11-16 the user additionally confirmed that the old application also receives pictures sent by another account in that VM, and both machines use the same WeChat version. Sender-account or WeChat-version differences therefore do not explain the reported remote failure. Offline tests below are code verification and cannot establish either machine's live behavior. See [manual Windows steps](TEST-SERVER-STEPS.md) for the separate scenarios.

The actual Rust `Media::inbound` path completed three existing pending messages without activating WeChat or changing the original conversation:

| Media | Verified bytes | Check |
| --- | ---: | --- |
| Original image | 56,233 | Original size and MD5, image decoding |
| Software/file attachment | 179,873,730 | Original size and MD5 |
| Standard video | 843,587 | Successful native completion, actual CDN size and matching server MD5 |

All three also received SHA-256 hashes in the adapter output. Real locators, resource IDs, keys, message bodies and personal files are excluded from this document and distribution. Offline adapter tests additionally cover cache-free image/file reception, resource-bound transcoded-video cache receipts and tamper rejection. Native tests cover type selection, 512 MiB bounds, malformed requests, completion/error decoding and rejection of unrelated callbacks.

## Operation and limits

Replace both the client and server packages and exit/restart both old applications to enable merged-forward cards and their separate detail window. Keep WeChat logged in. An identical already-loaded native build reconnects without a WeChat restart. A future different Media DLL cannot attach through an unknown existing hook chain: restart WeChat normally after that upgrade. Exact 4.1.15.13 verification remains mandatory.

Forwarding remains limited to selected groups and 512 MiB per item. A source that the CDN no longer supplies stays pending; this change cannot reconstruct unavailable source bytes. When the original video is unavailable, a verified standard/transcoded version may be delivered. The 512 MiB upper boundary is covered offline, not by a real 512 MiB transfer.

## WXGF 解码排查

本节保留原本分散在测试文档中的格式依据；用户操作流程只维护在 [本机测试步骤](TEST-SERVER-STEPS.md)。

真实 1080×2400 照片的一个已确认记录布局如下，偏移从文件开头计：

```text
33..38  00 00 01 3a 31    记录标记（NAL type 29，分区号 '1'）
38..42  00 01 3a 24       长度 0x13a24 = 80420
42..46  00 00 00 01       四字节起始码
46..    40 01 ...         VPS，随后 SPS / PPS / slice
```

`media/wxgf.rs::partitions` 扫描起始码，校验其前面的四字节长度、范围、参数集和单帧静图；无效候选被跳过。无长度前缀时另有 Annex-B 回退，颜色与透明遮罩按 chroma 区分。旧错误里的“声明 386096384 字节”来自误读头部，不能当作仍在下载的证据。

完整图片缺失或 WXGF 解码失败后，尝试同一消息的 `<md5>_t.dat` 或另一份 `<md5>.dat`；可解码缩略图写入 `preview.*`，在客户端先显示并继续重试完整图片。新代码不再发送灰色占位图；旧占位图常量仅用于识别旧缓存，不能当作真实图片缓存命中。旧灰块由服务端按固定指纹重新排队恢复，无缩略图或解码失败的图片保留重试。

FFmpeg 夹具代码验证：

```bash
export RUSTUP_HOME="$PWD/.tools/rustup" CARGO_HOME="$PWD/.tools/cargo"
export TMPDIR=/private/tmp
.tools/cargo/bin/cargo test -p wechat-adapter --bin wechat-adapter media::wxgf
```

真实 HEVC 夹具需要独立 FFmpeg；没有时只跳过依赖它的用例。这里的代码验证不能代替 Windows 真实接收结果。

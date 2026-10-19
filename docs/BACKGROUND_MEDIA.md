# Background media downloads (Windows WeChat 4.1.15.13)

On adapter reconnect, the relay queues existing legacy record summaries by their exact native source, including cards outside the 40-message warm window. The queue is bounded per loop and follows current group selection. Successfully reread attachments can also repair an older card that was incorrectly marked complete with unavailable media. Existing message IDs and sequence positions remain stable.

Merged-forward cards are also decoded from `recorditem/recordinfo/datalist`. The screenshot's real three-message record yielded two complete images of 52,342 and 5,006 bytes, verified against `datasize/fullmd5`; neither thumbnail resource was used. The client displays a clickable card and ordered, read-only details. Text remains visible while attachments download and the relay withholds its receipt until they finish. All attachment IDs remain owned by the outer group. Both client and server must be updated for the detail view. Creating or sending a new merged-forward card is outside this receiving change.

The download allowlist now follows all administrator-selected groups independently of native sending. Unopened groups can download without activating either chat UI. Removing a group or shutting down revokes its download grant. Unfinished record attachments use the normal short media retry schedule; history pages durably queue cards beyond their immediate conversion budget so they cannot be skipped by pagination.

The previously verified 843,587-byte standard video is H.264, 720×1280, 30 fps, 13.44 seconds, with AAC audio. It is a complete compressed/transcoded playback copy, not proof of the sender's original quality. No WeChat setting is needed to raise the old native endpoint's 30 MiB application limit; the new component accepts up to 512 MiB. The 512 MiB boundary was checked offline, while the real large-file sample was 179,873,730 bytes.

The previous adapter read image and attachment caches but never requested their download. Opening the original chat caused WeChat to populate those caches, which made forwarding appear to depend on the foreground window. The legacy video endpoint additionally imposed a 30 MiB limit while the adapter advertised 512 MiB.

The new `WeChatMedia` module calls WeChat's existing CDN task API. It uses WeChat's real constructor/destructor, allocator and live callback owner; the adapter checks the exact client and component hashes. It writes private partial files without opening chats or activating windows. Modern `7f0c` file IDs require BigFile type 7; treating them as ordinary type-5 attachments never completed in the observed failures.

Video has a separate failure mode. A real message declared 520,420 bytes, but a successful native CDN download returned a complete 843,587-byte video with a different MD5. The native completion callback supplied the actual size and an MD5 matching the downloaded bytes. Its original-video request returned error -5103017; that code is retained as an error, without assuming a specific undocumented meaning. Relaxing XML checks or treating a stable partial-file size as completion would be incorrect.

The adapter therefore prefers an original video when available and also permits the standard video to complete independently. Only a successful same-request native receipt can authenticate a transcoded standard video's different size/hash. Cache receipts are bound to the group, source locator, media kind, resource ID, AES parameters and declared metadata through a SHA-256 request ID. Every subsequent read rechecks actual bytes against the receipt. Ordinary images and software/file attachments continue to require their original size and MD5. Merged-forward images requested from cdndataurl with full-image type 1 can also be reencoded by the CDN: only a successful same-request completion receipt, actual size and matching CDN MD5 permit those replacement bytes. The decoded image is then validated and cached against the original resource fingerprint; thumbnail URLs and type 2 are not used. The six-message sample produced two complete PNG images of 1535×974 and 2540×1199 pixels (72,907 and 76,976 bytes).

The native listener is loopback-only with a random bearer token, selected-group checks on every request, bounded request/receipt counts, stable request fingerprints and private hash-derived destinations. Stop/deselection prevents new downloads. Completion observation is limited to already-registered request IDs and delegates unchanged to WeChat's original callback. No new chat messages were sent during diagnosis or download verification. Frida was used only as temporary developer instrumentation to confirm the result layout; it was detached and is absent from the release runtime.

## Verification

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

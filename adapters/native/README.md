This directory builds two independent modules for the version-verified Windows
bridge: `WeChatActions.dll` retains legacy observation/status diagnostics but
unconditionally disables its undocumented recall queue; `WeChatQuote.dll` creates a native type-57 text reference using verified
same-group database fields. Quote installs no observation hooks. Neither module
loads itself into any process; the Rust adapter performs WeChat version/hash
checks and explicit loading. The external `nen/work/WeChat-Hook` checkout is
unchanged. Each Actions and Quote build has its own pinned binary fingerprint;
packages and the Rust loader must use the matching fingerprint files.

Build and offline verification, from the workspace root:

```powershell
& adapters/native/build.ps1
```

Equivalent commands:

```powershell
.tools/Scripts/cmake.exe -S adapters/native -B build/native -G 'Visual Studio 18 2026' -A x64
.tools/Scripts/cmake.exe --build build/native --config Release --parallel 2
.tools/Scripts/ctest.exe --test-dir build/native -C Release --output-on-failure
```

Outputs are `build/native/Release/WeChatActions.dll`, `WeChatQuote.dll`,
`ActionsSelftest.exe` and `QuoteSelftest.exe`. CTest runs `actions-offline` and
`quote-offline` as two executables; their internal assertions are not counted as
extra functional cases. Both self-tests have assertions explicitly enabled even in Release, and a
preprocessor error prevents accidentally compiling it with `NDEBUG`. The test
uses synthetic in-memory records, a temporary config and stub record copies.
It never opens WeChat, loads a DLL, binds a listener, sends or recalls a message.
The DLLs use `/Brepro` for deterministic link output. All required third-party
source headers and MinHook C sources/licenses are included here.

The Rust loader places the DLL in its own
`data/adapter/native-actions/<binary-hash>/` directory. That directory's
`native.json` has the following schema:

```json
{
  "token": "64 lowercase hexadecimal characters",
  "port": 30111,
  "self_wxid": "wxid_current_account",
  "groups": ["12345678901@chatroom"]
}
```

The loader obtains `self_wxid` from the verified current-account database,
generates the token/loopback port, writes the config atomically and pins the DLL
SHA256. Other fields in the loader config are tolerated. The HTTP server binds
only `127.0.0.1`; every route requires `Authorization: Bearer <token>`.

Actions API version 1:

| Route | Request | Response |
| --- | --- | --- |
| `GET /health` | None | `pid`, `version="4.1.15.13"`, `api_version=1`, `capabilities=["recall_disabled","record_status"]`, `enabled`, `observed`, `records`, `selected_groups`, `window_seconds=180`; optional `error` prevents use. |
| `POST /start` | `{}` | Revalidate current config, enable observation for selected groups, return `{"enabled":true}`. |
| `POST /stop` | `{}` | Disable observation, release copied records, return `{"disabled":true,"record_memory_released":true}`. |
| `POST /recall` | `group`, decimal-string `server_id`, integer `create_time` seconds, integer `local_id`, 64-hex `request_id` | Always HTTP 400 `status="rejected"`: use the original WeChat UI menu. The implementation contains no native recall queue call. |
| `POST /record-status` | `group`, decimal-string `server_id`, integer `create_time` seconds, integer `local_id` | `ready`, `group`, `server_id`, `create_time`, `local_id`. Checks the existing cache only, without constructing, copying, expiring, destroying or queuing native objects. No pointers are returned. |
| `GET /result?request_id=<id>&group=<group>` | The original request ID and group | `server_id`, `request_id`, boolean `executed`, `processed`, `success`. Unknown or mismatched requests return 404. |

This legacy module is not an enabled forwarding recall implementation. There is
currently no verified background recall path, and the bridge does not fall back
to foreground UI automation. Users can recall manually in original WeChat.
Loading the optional diagnostic module creates
hooks with record observation disabled. The same resident module supports `/stop` followed by `/start`; hooks are never
unloaded while callbacks might use them. Configuration is reread every second
and synchronously before every action. Deselected groups' copied records are
released. A changed account or token disables actions and clears records.

A cached source must have a native record observed in this process, belong
to the current account and currently selected group, have a nonzero server ID,
and match the requested local ID and creation time. Time must satisfy
`0 <= now - create_time < 180` seconds. No native record is reconstructed from
database fields. Records absent from observation report `ready=false`; presence
does not enable recall submission. At most 32 copied records are retained, and they expire
after three minutes. The module exposes no message bodies.

The observer ignores the reference factory's source-copy callsite
`Weixin.dll+0x24EB6F8`; its exact native call bytes are verified before hooks are
enabled. Thus a bridge-generated quote source cannot replace an existing real
UI/database source through that callsite. Cached records must also contain the
readable, live shared DB-clone pair at `+0x2C0/+0x2C8`, as transferred by the
native DB-to-record conversion. These are rejection checks, not proof that every
downstream recall context is complete. A `ready` response reports cache presence
and the documented checks only; it does not confirm a native recall is safe or
that a recall has occurred. Polling rereads token/account/group grants without
calling native constructors, copy/destructors or the action queue.

Users must currently use WeChat's original UI menu manually for an actual recall.
Default-constructing a DB message and supplying its visible fields is not an
approved substitute for that path. In particular, native quote construction and
nonempty DB-clone slots do not prove the associated runtime contexts required
by recall are populated. No `prepare-recall` endpoint is provided.

`POST /recall` is unconditionally rejected before constructing tasks, reserving
receipts or calling native queues, including when the cache reports `ready=true`.
The response is `status="rejected"` with a concise explanation that the native
interface is disabled and the original WeChat menu is required. `/result` keeps
the legacy read-only receipt format for checking an already recorded outcome;
it cannot submit, resume, requeue, transfer another DLL's in-memory receipts or
declare an unknown result successful. Unknown receipt IDs return 404. Existing
uncertain attempts must remain uncertain until the matching actual
`sysmsg/revokemsg` is confirmed in the database. Only then may clients clear
the message content.

Only an exact successfully parsed request whose ID has never been reserved and
whose `(group, server_id)` has never been submitted can additionally carry
`definite_not_submitted=true` on a validation rejection. Callers may use that
explicit proof to distinguish a rejection from an uncertain historical attempt;
it does not enable background recall or authorize UI automation.
An old ID with a changed locator, a new ID for an already submitted message,
malformed input, native worker failure or an unknown result never carries that
proof. Repeated or uncertain submissions must still use their saved receipt.

Hook upgrades trust only the exact original native entries or the two explicitly
fingerprinted historical builds: the original `df39db26...` component and shipped
Actions `8be08c6e...`. Each existing MinHook relay must point into the expected
loaded image with its exact full-file hash, filename, callback RVA and prefix;
the image is pinned while its trampoline is retained. Unknown builds or modified
chains require a normal WeChat restart rather than broader hook trust. The
offline tests cover both known profiles, rejection of private/modified targets,
source-copy pollution, missing DB ownership, read-only polling and the strict
no-submission retry proof. They also assert that valid locators, an enabled cache,
repeated requests and synthetic historical uncertain receipts can never create
an action attempt or enter the disabled native queue.

The quote loader uses a different root,
`data/adapter/native-quotes/<binary-hash>/native.json`, with its own token/port,
the current `self_wxid`, selected `groups` and `capability="quote_text"`. It does
not share the Actions listener, authorization or observed-record cache.

Quote API version 1:

| Route | Request | Response |
| --- | --- | --- |
| `GET /health` | None | `pid`, `version="4.1.15.13"`, `capabilities=["quote_text"]`, `source_types=[1]`, `hooks_installed=false`, `enabled`, `selected_groups`; optional `error` prevents use. |
| `POST /start` | `{}` | Revalidate config and run the native record/clone layout probe without sending; return `enabled=true`. |
| `POST /stop` | `{}` | Disable further submissions; the resident module is not unloaded. |
| `POST /quote` | `group`, `source`, nonempty reply `text`, optional comma-separated `atlist`, 64-hex `request_id` | A stable `queued` receipt with `source_server_id`, `request_id`, `uuid`; ambiguous submission returns `uncertain`. |
| `GET /result?request_id=<id>&group=<group>` | The original request ID and selected group | Existing local queue receipt only; unknown or mismatched IDs return 404. This never confirms delivery. |

`source` contains `group`, `type=1`, confirmed decimal-string `server_id`, integer
`local_id`, epoch-second `create_time`, `fromusr`, `tousr`, `chatusr` and the exact
source `content`. Rust reconstructs these fields only after rereading the same
database locator. Own sources use `fromusr=self,tousr=group,chatusr=self`;
incoming sources use `fromusr=group,tousr=self,chatusr=the verified Name2Id actor`.
Media and nested reference sources are rejected; there is no summary fallback.

The module uses the version-specific native constructor/clone and type-57
factory, queues the result once, and persists its stable in-process receipt.
The Rust adapter persists its own attempt before submission. Duplicate IDs with
changed source/reply fields fail; lost replies trigger confirmation checks,
never blind requeueing. The adapter confirms a new nonzero server ID and the
actual direct appmsg type-57 title and referenced server/from/actor/content in
the database. Real reference `displayname` may be empty. XML line-ending
normalization is a confirmation concern and must not mutate the original text
passed to the native factory.

Offline build/test commands perform no runtime integration with WeChat. Real
native quote/recall/reedit acceptance is recorded separately in the public
`artifacts/native-quote-reedit-live-check.json`; old summary-text quote results
are migration history. All native layouts remain specific to 4.1.15.13.
DLL/WeChat fingerprint mismatches remain hard failures in the Rust loader.

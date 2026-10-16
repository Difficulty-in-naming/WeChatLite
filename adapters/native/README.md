This directory builds two independent modules for the version-verified Windows
bridge: `WeChatActions.dll` recalls a recent own message using an observed native
record; `WeChatQuote.dll` creates a native type-57 text reference using verified
same-group database fields. Quote installs no observation hooks. Neither module
loads itself into any process; the Rust adapter performs WeChat version/hash
checks and explicit loading. The external `nen/work/WeChat-Hook` checkout is
unchanged. The existing Actions binary retains its independent fingerprint.

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
| `GET /health` | None | `pid`, `version="4.1.15.13"`, `api_version=1`, `capabilities=["recall"]`, `enabled`, `observed`, `records`, `selected_groups`, `window_seconds=180`; optional `error` prevents use. |
| `POST /start` | `{}` | Revalidate current config, enable observation for selected groups, return `{"enabled":true}`. |
| `POST /stop` | `{}` | Disable observation, release copied records, return `{"disabled":true,"record_memory_released":true}`. |
| `POST /recall` | `group`, decimal-string `server_id`, integer `create_time` seconds, integer `local_id`, 64-hex `request_id` | `status="queued"`, `server_id`, `request_id`. |
| `GET /result?request_id=<id>&group=<group>` | The original request ID and group | `server_id`, `request_id`, boolean `executed`, `processed`, `success`. Unknown or mismatched requests return 404. |

Loading creates hooks with record observation disabled. The Rust adapter calls
`/start` during authorization before submitting newly requested messages. The
same resident module supports `/stop` followed by `/start`; hooks are never
unloaded while callbacks might use them. Configuration is reread every second
and synchronously before every action. Deselected groups' copied records are
released. A changed account or token disables actions and clears records.

A source must have a complete native record observed in this process, belong
to the current account and currently selected group, have a nonzero server ID,
and match the requested local ID and creation time. Time must satisfy
`0 <= now - create_time < 180` seconds. No native record is reconstructed from
database fields. Records absent from observation are rejected before submission,
with an explicit reason. At most 32 copied records are retained, and they expire
after three minutes. The module exposes no message bodies.

Before entering the native action queue, the module retains a receipt under
`request_id` and `(group, server_id)`. Repeating the same ID and exact locator
returns the prior queued receipt without entering the queue again. Reusing an ID
for a different locator or submitting the same source under a different ID is
rejected. Up to 1,024 receipts remain until WeChat exits, including after `/stop`.
The Rust adapter persists its own receipt before calling this API to preserve
that rule across adapter restarts.

HTTP 400 means validation rejected this request before a new native submission;
the JSON response includes `status="rejected"` and its precise `error`. A failure
after receipt reservation is HTTP 500 with `status="uncertain"`; callers must
poll existing confirmation rather than resubmit. Native `processed/success`
only describes the worker result. The Rust adapter must also reread the matching
database message and verify the expected `sysmsg/revokemsg` before announcing
successful recall or clearing content in clients.

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

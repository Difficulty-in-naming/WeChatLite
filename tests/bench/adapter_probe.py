#!/usr/bin/env python3
"""Baseline/after probe for the real adapter's per-cycle snapshot cost. No WeChat access.

Appends a temporary test to the adapter crate only for the duration of the run,
measures, then restores the original file byte-for-byte.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DB = os.path.join(ROOT, "adapters/windows-rs/src/db.rs")
MARKER = "// BENCH_PROBE (temporary)"


PROBE = '''

// BENCH_PROBE (temporary)
#[test]
fn bench_probe_snapshot_cost_per_cycle() {
    use std::time::Instant;
    let (_directory, mut db, connection, key) = encrypted_fixture();
    // Real WeChat shards are hundreds of MB. A 40 KB fixture would measure
    // syscall overhead, not the per-cycle full decrypt that dominates there.
    let rows = 120_000usize;
    let body = "x".repeat(900);
    let table = format!("Msg_{:x}", md5::Md5::digest(b"g@chatroom"));
    {
        let tx = connection.unchecked_transaction().unwrap();
        {
            let mut insert = connection
                .prepare(&format!(
                    "INSERT INTO {table}(local_id,local_type,real_sender_id,create_time,message_content,source,server_id,sort_seq) VALUES(?1,1,2,1700000000,?2,'',?3,?4)"
                ))
                .unwrap();
            for index in 0..rows {
                insert
                    .execute(rusqlite::params![
                        100 + index as i64,
                        body,
                        1000 + index as i64,
                        index as i64
                    ])
                    .unwrap();
            }
        }
        tx.commit().unwrap();
    }
    std::fs::write(
        &db.files["message/message_0.db"],
        crate::db::crypto::tests::encrypt(&connection.serialize("main").unwrap(), &key),
    )
    .unwrap();
    let size = std::fs::metadata(&db.files["message/message_0.db"]).unwrap().len();

    let cursor = json!({"sort_seq":0,"shard":"","local_id":-1});

    // Cold: one snapshot, as the adapter does on its first cycle.
    let started = Instant::now();
    let page = db.page("g@chatroom", &cursor, 2000).unwrap();
    let cold = started.elapsed();
    assert_eq!(page.len(), 2000);

    // Warm, no release: reuse the in-memory snapshot.
    let started = Instant::now();
    let rounds = 10;
    for _ in 0..rounds {
        db.page("g@chatroom", &cursor, 2000).unwrap();
    }
    let warm = started.elapsed() / rounds;

    // Warm, with release(): the old Engine::cycle() behaviour, rebuilding everything.
    let started = Instant::now();
    for _ in 0..rounds {
        db.release();
        db.page("g@chatroom", &cursor, 2000).unwrap();
    }
    let released = started.elapsed() / rounds;

    // Warm, with release_changed(): the new cycle behaviour, unchanged shard stays warm.
    let started = Instant::now();
    for _ in 0..rounds {
        db.release_changed().unwrap();
        db.page("g@chatroom", &cursor, 2000).unwrap();
    }
    let changed = started.elapsed() / rounds;

    println!(
        "BENCH_PROBE {{\\"db_bytes\\":{},\\"rows\\":{},\\"cold_ms\\":{:.2},\\"warm_ms\\":{:.2},\\"released_ms\\":{:.2},\\"changed_ms\\":{:.2},\\"penalty\\":{:.1},\\"changed_penalty\\":{:.1}}}",
        size,
        rows,
        cold.as_secs_f64() * 1000.0,
        warm.as_secs_f64() * 1000.0,
        released.as_secs_f64() * 1000.0,
        changed.as_secs_f64() * 1000.0,
        released.as_secs_f64() / warm.as_secs_f64().max(1e-9),
        changed.as_secs_f64() / warm.as_secs_f64().max(1e-9)
    );
}
'''


def run(cmd, env):
    result = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    return result


def main():
    env = dict(os.environ)
    env["RUSTUP_HOME"] = os.path.join(ROOT, ".tools/rustup")
    env["CARGO_HOME"] = os.path.join(ROOT, ".tools/cargo")
    env["PATH"] = os.path.join(ROOT, ".tools/cargo/bin") + ":" + env.get("PATH", "")

    original = open(DB, "rb").read()
    if MARKER.encode() in original:
        raise SystemExit("probe already present; restore db.rs first")
    # The probe needs the module-private fixture helpers, so it must live inside
    # `mod tests` -- insert before that module's final closing brace.
    text = original.decode()
    tail = text.rstrip()
    if not tail.endswith("}"):
        raise SystemExit("unexpected db.rs tail; refusing to insert probe")
    patched = tail[:-1] + PROBE + "\n}\n"
    try:
        with open(DB, "w") as handle:
            handle.write(patched)
        result = run(
            ["cargo", "test", "--release", "-p", "wechat-adapter", "--locked",
             "bench_probe_snapshot_cost_per_cycle", "--", "--nocapture"],
            env,
        )
        output = result.stdout + result.stderr
        match = re.search(r"BENCH_PROBE (\{.*\})", output)
        if not match:
            print(output[-4000:])
            raise SystemExit("probe produced no measurement")
        print(json.dumps(json.loads(match.group(1)), indent=2))
        with open(os.path.join(os.environ.get("BENCH_RESULTS", os.path.join(HERE, "results")), "adapter_probe_%s.json" % sys.argv[1]), "w") as handle:
            handle.write(match.group(1))
    finally:
        with open(DB, "wb") as handle:
            handle.write(original)


if __name__ == "__main__":
    main()

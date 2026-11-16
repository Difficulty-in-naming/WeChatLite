#!/usr/bin/env python3
"""Measure the relay's single-in-flight queue behaviour. No WeChat access.

Temporarily appends a test to server/src/bridge.rs, runs it, restores the file.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TARGET = os.path.join(ROOT, "server/src/bridge.rs")
MARKER = "// BENCH_PROBE (temporary)"

PROBE = '''

    // BENCH_PROBE (temporary)
    #[tokio::test]
    async fn bench_probe_single_inflight_blocks_the_queue() {
        use std::time::Instant;
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let ids = ["head", "second", "third", "fourth"];
        for id in ids {
            app.store.insert_message(&mut message(id)).unwrap();
        }
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;

        // Drain the initial configure/status exchanges so timing starts clean.
        assert_eq!(next(&mut input).await["message"]["id"], "head");

        let started = Instant::now();
        let mut dispatched = 1usize;
        let mut probe = tokio::time::interval(Duration::from_millis(100));
        let mut queue_after_ack_ms = None;

        // Observe: while `head` stays unacknowledged, nothing else may be dispatched.
        tokio::time::timeout(Duration::from_millis(900), async {
            loop {
                probe.tick().await;
                match tokio::time::timeout(Duration::from_millis(5), input.next()).await {
                    Ok(Some(Ok(_))) => dispatched += 1,
                    _ => {}
                }
            }
        })
        .await
        .ok();

        let blocked = Instant::now().duration_since(started);
        let statuses_before: Vec<String> = ids
            .iter()
            .map(|id| app.store.message(id).unwrap().unwrap().status)
            .collect();

        emit(
            &mut output,
            json!({"type":"ack","id":"head","status":"sent","source_id":"wx:g:bench:bench:1"}),
        )
        .await;

        // The next message must be released promptly once the head is confirmed.
        let release_started = Instant::now();
        if let Ok(Some(Ok(line))) = tokio::time::timeout(Duration::from_secs(2), input.next()).await {
            let value: serde_json::Value = serde_json::from_str(&line).unwrap();
            if value["op"] == "send" {
                queue_after_ack_ms = Some(release_started.elapsed().as_millis());
            }
        }

        println!(
            "BENCH_PROBE {{\\"dispatched_while_head_pending\\":{},\\"blocked_ms\\":{},\\"queue_after_ack_ms\\":{},\\"statuses_before\\":{}}}",
            dispatched,
            blocked.as_millis(),
            queue_after_ack_ms
                .map(|v| v.to_string())
                .unwrap_or_else(|| "null".into()),
            serde_json::to_string(&statuses_before).unwrap()
        );
        task.abort();
    }
'''


def main():
    env = dict(os.environ)
    env["RUSTUP_HOME"] = os.path.join(ROOT, ".tools/rustup")
    env["CARGO_HOME"] = os.path.join(ROOT, ".tools/cargo")
    env["PATH"] = os.path.join(ROOT, ".tools/cargo/bin") + ":" + env.get("PATH", "")

    original = open(TARGET, "rb").read()
    if MARKER.encode() in original:
        raise SystemExit("probe already present; restore bridge.rs first")
    text = original.decode()
    tail = text.rstrip()
    if not tail.endswith("}"):
        raise SystemExit("unexpected bridge.rs tail; refusing to insert probe")
    try:
        with open(TARGET, "w") as handle:
            handle.write(tail[:-1] + PROBE + "\n}\n")
        result = subprocess.run(
            [ "cargo", "test", "-p", "wechat-relay", "--locked",
             "bench_probe_single_inflight_blocks_the_queue", "--", "--nocapture"],
            cwd=ROOT, env=env, capture_output=True, text=True,
        )
        output = result.stdout + result.stderr
        match = re.search(r"BENCH_PROBE (\{.*\})", output)
        if not match:
            print(output[-5000:])
            raise SystemExit("probe produced no measurement")
        print(json.dumps(json.loads(match.group(1)), indent=2))
        with open(os.path.join(os.environ.get("BENCH_RESULTS", os.path.join(HERE, "results")), "bridge_probe_%s.json" % sys.argv[1]), "w") as handle:
            handle.write(match.group(1))
    finally:
        with open(TARGET, "wb") as handle:
            handle.write(original)


if __name__ == "__main__":
    main()

use std::{
    io::{Read, Write},
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

fn run_configuration(configuration: &str) -> (std::process::ExitStatus, String) {
    let mut child = Command::new(env!("CARGO_BIN_EXE_wechat-link"))
        .arg("--client")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    writeln!(input, "{configuration}").unwrap();
    drop(input); // The helper must exit during connection setup when its owner closes stdin.
    let deadline = Instant::now() + Duration::from_secs(6);
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            panic!("helper did not exit after stdin EOF");
        }
        thread::sleep(Duration::from_millis(10));
    };
    let mut output = String::new();
    child
        .stdout
        .take()
        .unwrap()
        .read_to_string(&mut output)
        .unwrap();
    for line in output.lines() {
        assert!(line.len() <= 1024);
        let _: serde_json::Value = serde_json::from_str(line).unwrap();
    }
    (status, output)
}

#[test]
fn stdin_eof_cancels_an_unreachable_private_relay_without_waiting_for_connect() {
    let directory = tempfile::tempdir().unwrap();
    let value = serde_json::json!({
        "server_id": iroh::SecretKey::from_bytes(&[7;32]).public().to_string(),
        "relay_url":"http://127.0.0.1:9/", "relay_token":"synthetic-relay-secret",
        "relay_ca":"", "relay_quic_port":9, "relay_only":true,
        "key_file":directory.path().join("identity.key"),
    });
    let (status, output) = run_configuration(&value.to_string());
    assert!(status.success(), "EOF should be a normal owned shutdown");
    assert!(!output.contains("synthetic-relay-secret"));
    assert_eq!(
        std::fs::read(directory.path().join("identity.key"))
            .unwrap()
            .len(),
        32
    );
}

#[test]
fn malformed_configuration_emits_only_bounded_generic_ndjson() {
    let (status, output) =
        run_configuration(r#"{"server_id":"invalid","relay_token":"DO-NOT-LOG-THIS-TOKEN"}"#);
    assert!(!status.success());
    assert!(!output.contains("DO-NOT-LOG-THIS-TOKEN"));
    assert!(output.contains("\"type\":\"error\""));
}

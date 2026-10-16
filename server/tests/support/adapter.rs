//! The integration test executable doubles as a deliberately untrusted, local NDJSON adapter.
use anyhow::{Context, Result};
use serde_json::{Value, json};
use std::{
    fs::OpenOptions,
    io::{self, BufRead, Write},
    path::Path,
};

fn emit(value: Value) -> Result<()> {
    let mut output = io::stdout().lock();
    serde_json::to_writer(&mut output, &value)?;
    writeln!(output)?;
    output.flush()?;
    Ok(())
}
fn source(group: &str, index: i64) -> String {
    format!("wx:{group}:message__message_0.db:{index}:{index}")
}
fn history_message(group: &str, index: i64) -> Value {
    json!({"id":source(group,index),"source_id":source(group,index),"group":group,"sender":"Offline member","sender_id":"offline-actor","text":format!("Offline row {index}"),"timestamp":1700000000000i64+(index/7)*1000,"status":"received","media":[]})
}
fn history_page(directory: &Path, command: &Value) -> Result<()> {
    let group = command["group"].as_str().unwrap();
    if directory.join("history-error").exists() {
        return emit(
            json!({"type":"history_done","request_id":command["request_id"],"group":group,"error":"Offline read failed"}),
        );
    }
    let before = command["before_source"].as_str().unwrap_or("");
    let start = if before.is_empty() {
        138
    } else {
        before.rsplit(':').next().unwrap().parse::<i64>()?
    };
    let rows = (1..start.min(138)).rev().collect::<Vec<_>>();
    let chunk = rows.iter().take(60).copied().collect::<Vec<_>>();
    for index in chunk.iter().rev() {
        emit(json!({"type":"message","historical":true,"message":history_message(group,*index)}))?;
    }
    emit(
        json!({"type":"history_done","request_id":command["request_id"],"group":group,"before_source":chunk.last().map(|n|source(group,*n)).unwrap_or(before.to_owned()),"has_more":rows.len()>60,"sources":chunk.iter().map(|n|source(group,*n)).collect::<Vec<_>>()}),
    )
}
fn announcement(directory: &Path, command: &Value) -> Result<()> {
    let available = !directory.join("announcement-unavailable").exists();
    let set = available && !directory.join("announcement-empty").exists();
    emit(
        json!({"type":"announcement","request_id":command["request_id"],"group":command["group"],"available":available,"set":set,"text":if set{"Offline announcement"}else{""},"author_id":if set{"offline-author"}else{""},"author":if set{"Offline author"}else{""},"avatar":"","updated_at":if set{Some(1700000000123i64)}else{None},"path":"private-not-public"}),
    )
}
pub fn run(mode: &str, directory: &Path) -> Result<()> {
    if mode == "startup-sentinel" {
        std::fs::write(directory, b"unexpected-start")?;
        return Ok(());
    }
    std::fs::write(
        directory.join("initial-policy.json"),
        std::env::var("WECHAT_LITE_FORWARD_GROUPS").context("missing fixture policy")?,
    )?;
    if mode != "shutdown" {
        emit(
            json!({"type":"groups","groups":[{"id":"fixture-a","name":"Private catalog fixture-a","count":2},{"id":"fixture-b","name":"Private catalog fixture-b","count":2}]}),
        )?;
    }
    emit(json!({"type":"status","ready":true,"text":"Offline Rust fixture"}))?;
    let mut held: Option<Value> = None;
    let mut counter = 0;
    let mut held_history = Vec::new();
    let mut held_announcements = Vec::new();
    for line in io::stdin().lock().lines() {
        let command: Value = serde_json::from_str(&line?)?;
        let mut log = OpenOptions::new()
            .create(true)
            .append(true)
            .open(directory.join("commands.jsonl"))?;
        let mut encoded = serde_json::to_vec(&command)?;
        encoded.push(b'\n');
        log.write_all(&encoded)?;
        if !directory.join("hold-history").exists() {
            for pending in held_history.drain(..) {
                history_page(directory, &pending)?;
            }
        }
        if !directory.join("hold-announcement").exists() {
            for pending in held_announcements.drain(..) {
                announcement(directory, &pending)?;
            }
        }
        match command["op"].as_str().unwrap_or("") {
            "configure" => {
                for group in command["groups"].as_array().unwrap() {
                    emit(
                        json!({"type":"members","group":group,"members":[{"id":format!("member-{}",group.as_str().unwrap()),"name":format!("Member {}",group.as_str().unwrap())}]}),
                    )?;
                }
                if held.as_ref().is_some_and(|row| {
                    !command["groups"]
                        .as_array()
                        .unwrap()
                        .contains(&row["group"])
                }) {
                    let row = held.take().unwrap();
                    emit(
                        json!({"type":"ack","id":row["id"],"status":"sent","source_id":format!("ack-{}",row["id"].as_str().unwrap())}),
                    )?;
                }
            }
            "activate" if mode == "history" => {
                for index in 98..138 {
                    emit(
                        json!({"type":"message","message":history_message(command["group"].as_str().unwrap(),index)}),
                    )?;
                }
            }
            "activate" => {
                counter += 1;
                for group in ["fixture-a", "fixture-b"] {
                    let path = directory.join(format!("{group}.dat"));
                    let bytes = format!("private bytes {group}");
                    std::fs::write(&path, &bytes)?;
                    let id = format!("{group}-{counter}");
                    emit(
                        json!({"type":"message","message":{"id":id,"source_id":id,"group":group,"sender":format!("Member {group}"),"text":format!("Private body {group}"),"timestamp":chrono::Utc::now().timestamp_millis(),"status":"received","media":[{"id":format!("native-{id}"),"name":format!("{group}.dat"),"path":path,"kind":"file","size":bytes.len(),"sha256":""}]}}),
                    )?;
                }
            }
            "send" => {
                let row = &command["message"];
                let id = row["id"].as_str().unwrap();
                if row["text"].as_str().unwrap_or("").starts_with("hold") {
                    held = Some(row.clone());
                } else {
                    emit(
                        json!({"type":"ack","id":id,"status":"sent","source_id":format!("ack-{id}"),"source_ids":[format!("ack-{id}"),format!("second-part-{id}")]}),
                    )?;
                }
            }
            "recall" | "recall_check" => {
                let row = &command["message"];
                let text = row["text"].as_str().unwrap_or("");
                if command["op"] == "recall" && text.starts_with("recall hold") {
                    continue;
                }
                let status = if text.starts_with("recall fail") {
                    "failed"
                } else if text.starts_with("recall uncertain") {
                    "uncertain"
                } else {
                    "recalled"
                };
                emit(
                    json!({"type":"recall_ack","id":row["id"],"status":status,"error":if status=="recalled"{""}else{"Fixture confirmation unavailable"}}),
                )?;
            }
            "history" => {
                if directory.join("hold-history").exists() {
                    held_history.push(command);
                } else {
                    history_page(directory, &command)?;
                }
            }
            "announcement" => {
                if directory.join("hold-announcement").exists() {
                    held_announcements.push(command);
                } else {
                    announcement(directory, &command)?;
                }
            }
            _ => {}
        }
    }
    if mode == "shutdown" {
        emit(
            json!({"type":"status","ready":false,"text":format!("EOF tail {}","x".repeat(128*1024))}),
        )?;
        std::fs::write(directory.join("adapter-stopped"), "stdin-closed")?;
    }
    Ok(())
}

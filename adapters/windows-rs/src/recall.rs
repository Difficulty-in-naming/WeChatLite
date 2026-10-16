use crate::engine::{Backend, Receipts, Rejected, text};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

// A positive proof from local preconditions or the UI helper before menu Invoke.
// A generic Rejected/worker failure does not establish that nothing was queued.
#[derive(Debug)]
pub struct DefinitelyNotSubmitted;
impl std::fmt::Display for DefinitelyNotSubmitted {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("微信撤回未提交")
    }
}
impl std::error::Error for DefinitelyNotSubmitted {}

fn unsubmitted(record: &Value) -> bool {
    !matches!(text(&record["ack"], "status"), "uncertain" | "recalled")
        && record["index"] == 0
        && record["parts"].as_array().is_some_and(|parts| {
            !parts.is_empty()
                && parts.iter().enumerate().all(|(index, part)| {
                    part["submitted"] != true
                        || (index == 0 && part["definite_not_submitted"] == true)
                })
        })
}

pub struct RecallJob {
    id: String,
    attempt: String,
    group: String,
    record: Value,
    check_only: bool,
}
impl RecallJob {
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn group(&self) -> &str {
        &self.group
    }
    pub fn attempt(&self) -> &str {
        &self.attempt
    }
    pub fn new<A: Backend>(
        access: &mut A,
        store: &Receipts,
        message: &Value,
        check_only: bool,
        explicit_retry: bool,
        now: f64,
    ) -> Result<Self> {
        let id = text(message, "id").to_owned();
        let group = text(message, "group").to_owned();
        let attempt = text(message, "recall_attempt").to_owned();
        ensure!(
            attempt.is_empty()
                || (attempt.len() == 32 && attempt.bytes().all(|value| value.is_ascii_hexdigit())),
            "撤回轮次无效"
        );
        ensure!(
            check_only || !attempt.is_empty(),
            "旧版撤回只能查询结果，未重新执行"
        );
        ensure!(
            !id.is_empty() && id.len() <= 256 && !group.is_empty(),
            "撤回目标编号无效"
        );
        let mut sources: Vec<String> = message["source_ids"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|v| v.as_str().map(str::to_owned))
            .collect();
        if sources.is_empty() {
            if !text(message, "source_id").is_empty() {
                sources.push(text(message, "source_id").to_owned());
            }
        }
        if let Some(outgoing) = store.state(&format!("outgoing:{id}"))? {
            if let Some(ack_sources) = outgoing["ack"]["source_ids"].as_array() {
                let saved: Vec<_> = ack_sources
                    .iter()
                    .filter_map(|s| s.as_str().map(str::to_owned))
                    .collect();
                if !saved.is_empty() {
                    sources = saved;
                }
            }
        }
        ensure!(
            !sources.is_empty() && sources.len() <= 13,
            "撤回消息缺少微信确认来源"
        );
        let hash = format!(
            "{:x}",
            Sha256::digest(serde_json::to_vec(
                &json!({"group":group,"sources":sources})
            )?)
        );
        let key = format!("recall:{id}");
        let previous = store.state(&key)?;
        if let Some(record) = &previous {
            ensure!(record["hash"] == hash, "撤回目标发生变化，未重复提交");
        }
        let retry = explicit_retry
            && !check_only
            && previous.as_ref().is_some_and(|record| {
                record["ack"]["status"] == "failed"
                    && record["ack"]["definite_not_submitted"] == true
                    && unsubmitted(record)
            });
        let check_only = check_only
            || previous
                .as_ref()
                .is_some_and(|record| text(record, "recall_attempt") != attempt && !retry);
        let record = if let Some(record) = previous.filter(|_| !retry) {
            record
        } else {
            ensure!(!check_only, "撤回未提交，未发现本机持久记录");
            ensure!(
                message["outgoing"] == true
                    && matches!(text(message, "status"), "sent" | "received"),
                "只能撤回本人已确认的消息"
            );
            let stamp = message["timestamp"].as_f64().context("撤回消息时间无效")? / 1000.0;
            ensure!(now >= stamp && now - stamp < 180.0, "已超过3分钟撤回时限");
            let mut parts = vec![];
            // Resolve every part before submitting any recall request.
            for (index, source) in sources.iter().enumerate() {
                let info = access.recall_info(&group, source, now)?;
                let request_id = format!(
                    "{:x}",
                    Sha256::digest(format!("recall:{id}:{index}:{source}").as_bytes())
                );
                parts.push(json!({"info":info,"request_id":request_id}));
            }
            let record =
                json!({"hash":hash,"parts":parts,"index":0,"started":now,"recall_attempt":attempt});
            store.set(&key, &record)?;
            record
        };
        Ok(Self {
            id,
            attempt,
            group,
            record,
            check_only,
        })
    }
    fn save(&self, store: &Receipts) -> Result<()> {
        store.set(&format!("recall:{}", self.id), &self.record)
    }
    fn finish(&mut self, store: &Receipts, status: &str, error: &str) -> Result<Value> {
        let mut ack =
            json!({"type":"recall_ack","id":self.id,"recall_attempt":self.attempt,"status":status});
        if !error.is_empty() {
            ack["error"] = json!(error);
        }
        ack["definite_not_submitted"] =
            json!(status == "failed" && !self.attempt.is_empty() && unsubmitted(&self.record));
        self.record["ack"] = ack.clone();
        self.save(store)?;
        Ok(ack)
    }
    fn submitted(&self) -> bool {
        self.record["parts"].as_array().is_some_and(|parts| {
            parts
                .iter()
                .any(|p| p["submitted"] == true && p["rejected"] != true)
        })
    }
    pub fn advance<A: Backend>(
        &mut self,
        access: &mut A,
        store: &Receipts,
        allowed: bool,
        now: f64,
    ) -> Result<Option<Value>> {
        if let Some(current) = store.state(&format!("recall:{}", self.id))? {
            if text(&current, "recall_attempt") != self.attempt
                || current["recall_attempt"] != self.record["recall_attempt"]
            {
                // A newer explicit retry owns the durable receipt. An old check
                // must neither queue nor overwrite its unsubmitted state.
                return Ok(Some(
                    json!({"type":"recall_ack","id":self.id,"recall_attempt":self.attempt,
                    "status":"uncertain","definite_not_submitted":false,"error":"旧撤回轮次已结束，未重复执行"}),
                ));
            }
            self.record = current;
        }
        if let Some(ack) = self.record.get("ack") {
            let mut ack = ack.clone();
            ack["recall_attempt"] = json!(self.attempt);
            if self.attempt.is_empty() {
                ack["definite_not_submitted"] = json!(false);
            }
            return Ok(Some(ack));
        }
        let result = self.step(access, store, allowed, now);
        match result {
            Ok(true) => Ok(Some(self.finish(store, "recalled", "")?)),
            Ok(false) => Ok(None),
            Err(error) => {
                let status = if self.submitted() {
                    "uncertain"
                } else {
                    "failed"
                };
                Ok(Some(self.finish(store, status, &error.to_string())?))
            }
        }
    }
    fn step<A: Backend>(
        &mut self,
        access: &mut A,
        store: &Receipts,
        allowed: bool,
        now: f64,
    ) -> Result<bool> {
        let parts = self.record["parts"]
            .as_array()
            .context("保存的撤回记录无效")?;
        let index = self.record["index"].as_u64().context("撤回进度无效")? as usize;
        if index == parts.len() {
            return Ok(true);
        }
        ensure!(index < parts.len(), "撤回进度越界");
        let part = parts[index].clone();
        let info = &part["info"];
        let request_id = text(&part, "request_id");
        if part["submitted"] != true {
            ensure!(!self.check_only, "撤回的后续部分尚未提交，未自动执行");
            ensure!(allowed, "该群已取消转发，后续撤回未提交");
            let stamp = info["create_time"].as_f64().context("原消息时间无效")?;
            ensure!(
                now >= stamp && now - stamp < 180.0,
                "已超过3分钟撤回时限，未提交后续撤回"
            );
            self.record["parts"][index]["submitted"] = json!(true);
            self.record["parts"][index]["submitted_at"] = json!(now);
            self.save(store)?;
            if let Err(error) = access.recall_submit(&self.group, info, request_id) {
                if error.downcast_ref::<Rejected>().is_some() {
                    self.record["parts"][index]["rejected"] = json!(true);
                }
                if error.downcast_ref::<DefinitelyNotSubmitted>().is_some() {
                    self.record["parts"][index]["definite_not_submitted"] = json!(true);
                    self.record["parts"][index]["rejected"] = json!(true);
                }
                self.save(store)?;
                return Err(error);
            }
        }
        match access.recall_progress(&self.group, info, request_id)? {
            Some(true) => {
                self.record["parts"][index]["confirmed"] = json!(true);
                self.record["index"] = json!(index + 1);
                self.save(store)?;
                Ok(index + 1 == self.record["parts"].as_array().unwrap().len())
            }
            Some(false) => {
                self.record["parts"][index]["rejected"] = json!(true);
                self.save(store)?;
                anyhow::bail!("微信后台拒绝了撤回，原消息保留")
            }
            None => {
                let submitted = self.record["parts"][index]["submitted_at"]
                    .as_f64()
                    .unwrap_or(now);
                ensure!(
                    now - submitted < 30.0,
                    "撤回已提交但尚未确认，原消息保留；未重复执行"
                );
                Ok(false)
            }
        }
    }
}

pub fn construction_failure(
    store: &Receipts,
    message: &Value,
    check_only: bool,
    error: &anyhow::Error,
) -> Result<Value> {
    let previous = store.state(&format!("recall:{}", text(message, "id")))?;
    let attempt = text(message, "recall_attempt");
    let definite = !check_only
        && attempt.len() == 32
        && attempt.bytes().all(|value| value.is_ascii_hexdigit())
        && previous.as_ref().is_none_or(|record| {
            (text(record, "recall_attempt") == attempt
                || (record["ack"]["status"] == "failed"
                    && record["ack"]["definite_not_submitted"] == true))
                && unsubmitted(record)
        });
    Ok(
        json!({"type":"recall_ack","id":message["id"],"recall_attempt":attempt,"status":"failed",
        "error":error.to_string(),"definite_not_submitted":definite}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn terminal_unknown_or_confirmed_ack_cannot_be_reclassified_from_missing_part_flags() {
        for status in ["uncertain", "recalled"] {
            let record = json!({"index":0,"parts":[{}],"ack":{"status":status}});
            assert!(!unsubmitted(&record));
        }
        assert!(unsubmitted(&json!({"index":0,"parts":[{}]})));
    }
}

use crate::engine::{Backend, Receipts, Rejected, text};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

pub struct RecallJob {
    id: String,
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
    pub fn new<A: Backend>(
        access: &mut A,
        store: &Receipts,
        message: &Value,
        check_only: bool,
        now: f64,
    ) -> Result<Self> {
        let id = text(message, "id").to_owned();
        let group = text(message, "group").to_owned();
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
        let record = if let Some(record) = store.state(&key)? {
            ensure!(record["hash"] == hash, "撤回目标发生变化，未重复提交");
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
            let record = json!({"hash":hash,"parts":parts,"index":0,"started":now});
            store.set(&key, &record)?;
            record
        };
        Ok(Self {
            id,
            group,
            record,
            check_only,
        })
    }
    fn save(&self, store: &Receipts) -> Result<()> {
        store.set(&format!("recall:{}", self.id), &self.record)
    }
    fn finish(&mut self, store: &Receipts, status: &str, error: &str) -> Result<Value> {
        let mut ack = json!({"type":"recall_ack","id":self.id,"status":status});
        if !error.is_empty() {
            ack["error"] = json!(error);
        }
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
        if let Some(ack) = self.record.get("ack") {
            return Ok(Some(ack.clone()));
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
                    self.save(store)?;
                }
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

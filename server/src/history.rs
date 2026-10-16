use crate::App;
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use tokio::sync::{Mutex, watch};

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Progress {
    #[serde(skip)]
    pub pending: bool,
    #[serde(default)]
    pub initialized: bool,
    #[serde(default)]
    pub before_source: String,
    #[serde(default)]
    pub exhausted: bool,
    #[serde(default)]
    pub sources: Vec<String>,
}

pub fn position(group: &str, source: &str) -> Option<(i64, String, i64)> {
    let rest = source.strip_prefix(&format!("wx:{group}:"))?;
    let (rest, sequence) = rest.rsplit_once(':')?;
    let (shard, local) = rest.rsplit_once(':')?;
    if shard.is_empty() || shard.len() > 256 || shard.chars().any(char::is_control) {
        return None;
    }
    Some((
        sequence.parse().ok()?,
        shard.to_owned(),
        local.parse().ok()?,
    ))
}

struct Pending {
    id: String,
    before_source: String,
    result: watch::Sender<Option<Value>>,
}

#[derive(Default)]
pub struct Requests {
    pending: Mutex<BTreeMap<String, Pending>>,
}

impl Requests {
    pub async fn request(
        &self,
        app: &App,
        group: &str,
        client: &str,
        tail: bool,
    ) -> Result<Option<watch::Receiver<Option<Value>>>> {
        let mut pending = self.pending.lock().await;
        if let Some(job) = pending.get(group) {
            return Ok(Some(job.result.subscribe()));
        }
        ensure!(pending.len() < 64, "历史读取任务较多，请稍后重试");
        let plan = app
            .db({
                let group = group.to_owned();
                move |store| store.history_progress(&group)
            })
            .await?;
        if plan.exhausted {
            return Ok(None);
        }
        let id = uuid::Uuid::new_v4().to_string();
        let (result, receiver) = watch::channel(None);
        let before_source = if tail && !plan.initialized {
            String::new()
        } else {
            plan.before_source
        };
        app.commands.try_send(json!({"op":"history","request_id":id,"group":group,"before_source":before_source,"limit":60,"_access_client":client}))
            .context("历史读取队列已满或本机微信已断开")?;
        pending.insert(
            group.to_owned(),
            Pending {
                id,
                before_source,
                result,
            },
        );
        Ok(Some(receiver))
    }

    pub async fn finish(&self, app: &App, value: Value) -> Result<()> {
        let group = value["group"].as_str().context("历史响应缺少群编号")?;
        let mut pending = self.pending.lock().await;
        let Some(job) = pending.get(group) else {
            return Ok(());
        };
        if Some(job.id.as_str()) != value["request_id"].as_str() {
            return Ok(());
        }
        if value["error"].as_str().unwrap_or("").is_empty() {
            let before_source = value["before_source"]
                .as_str()
                .context("历史响应缺少游标")?
                .to_owned();
            ensure!(
                before_source.is_empty() || position(group, &before_source).is_some(),
                "历史响应游标无效"
            );
            let has_more = value["has_more"]
                .as_bool()
                .context("历史响应缺少结束标记")?;
            if has_more {
                let next = position(group, &before_source).context("历史页未推进游标")?;
                ensure!(
                    position(group, &job.before_source).is_none_or(|previous| next < previous),
                    "历史页未向更早推进"
                );
            }
            let sources = value["sources"]
                .as_array()
                .context("历史响应缺少来源列表")?;
            ensure!(sources.len() <= 60, "历史响应过大");
            let sources = sources
                .iter()
                .map(|source| source.as_str().context("历史来源无效").map(str::to_owned))
                .collect::<Result<Vec<_>>>()?;
            ensure!(
                sources
                    .iter()
                    .all(|source| position(group, source).is_some()),
                "历史来源不属于该群"
            );
            let progress = Progress {
                pending: false,
                initialized: true,
                before_source,
                exhausted: !has_more,
                sources,
            };
            app.db({
                let group = group.to_owned();
                move |store| store.set_history_progress(&group, &progress)
            })
            .await?;
        }
        let job = pending.remove(group).unwrap();
        job.result.send_replace(Some(value));
        Ok(())
    }

    pub async fn fail_all(&self, error: &str) {
        for (group, job) in std::mem::take(&mut *self.pending.lock().await) {
            job.result.send_replace(Some(
                json!({"group":group,"request_id":job.id,"error":error}),
            ));
        }
    }
}

use crate::App;
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};
use tokio::sync::{Mutex, watch};

pub const TIMEOUT: Duration = Duration::from_secs(3);
struct Pending {
    id: String,
    started: Instant,
    result: watch::Sender<Option<Value>>,
}
#[derive(Default)]
pub struct Requests {
    pending: Mutex<BTreeMap<String, Pending>>,
}

pub fn unavailable(group: &str, pending: bool) -> Value {
    json!({"group":group,"available":false,"set":false,"author_id":"","author":"","avatar":"","updated_at":null,"text":"","pending":pending})
}
fn sanitized(value: &Value, group: &str) -> Value {
    let text = value["text"].as_str().unwrap_or("");
    let author_id = value["author_id"].as_str().unwrap_or("");
    let author = value["author"].as_str().unwrap_or("");
    let avatar = value["avatar"].as_str().unwrap_or("");
    if value["available"] != true
        || !value["set"].is_boolean()
        || text.len() > 64 * 1024
        || text.contains('\0')
        || author_id.len() > 1024
        || author_id.chars().any(char::is_control)
        || author.len() > 4096
        || author.contains('\0')
        || avatar.len() > 64 * 1024
        || !avatar
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"+/=".contains(&b))
    {
        return unavailable(group, false);
    }
    let set = value["set"] == true;
    json!({"group":group,"available":true,"set":set,
        "author_id":if set {author_id} else {""},"author":if set {author} else {""},
        "avatar":if set {avatar} else {""},
        "updated_at":if set {value["updated_at"].as_i64().filter(|n| *n>0)} else {None},
        "text":if set {text} else {""},"pending":false})
}
impl Requests {
    pub async fn request(
        &self,
        app: &App,
        group: &str,
        client: &str,
    ) -> Result<(String, watch::Receiver<Option<Value>>)> {
        let mut jobs = self.pending.lock().await;
        // Expiration is lazy and bounded, including HTTP callers that disconnected while waiting.
        jobs.retain(|group, job| {
            if job.started.elapsed() < TIMEOUT {
                true
            } else {
                job.result.send_replace(Some(unavailable(group, true)));
                false
            }
        });
        if let Some(job) = jobs.get(group) {
            return Ok((job.id.clone(), job.result.subscribe()));
        }
        ensure!(jobs.len() < 64, "公告读取任务较多，请稍后重试");
        let id = uuid::Uuid::new_v4().to_string();
        let (result, receiver) = watch::channel(None);
        app.commands
            .try_send(
                json!({"op":"announcement","group":group,"request_id":id,"_access_client":client}),
            )
            .context("本机微信未连接或公告读取队列已满")?;
        jobs.insert(
            group.to_owned(),
            Pending {
                id: id.clone(),
                started: Instant::now(),
                result,
            },
        );
        Ok((id, receiver))
    }
    pub async fn finish(&self, value: Value) {
        let Some(group) = value["group"].as_str() else {
            return;
        };
        let mut jobs = self.pending.lock().await;
        if !jobs
            .get(group)
            .is_some_and(|job| Some(job.id.as_str()) == value["request_id"].as_str())
        {
            return;
        }
        let job = jobs.remove(group).unwrap();
        let response = if job.started.elapsed() < TIMEOUT {
            sanitized(&value, group)
        } else {
            unavailable(group, true)
        };
        job.result.send_replace(Some(response));
    }
    pub async fn expire(&self, group: &str, id: &str) {
        let mut jobs = self.pending.lock().await;
        if jobs.get(group).is_some_and(|job| job.id == id) {
            let job = jobs.remove(group).unwrap();
            job.result.send_replace(Some(unavailable(group, true)));
        }
    }
    pub async fn fail_all(&self) {
        for (group, job) in std::mem::take(&mut *self.pending.lock().await) {
            job.result.send_replace(Some(unavailable(&group, false)));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn shared_request_expiration_and_generation_ignore_late_replies_and_connection_failures_settle()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut commands) = crate::test_app(temp.path());
        let (first_id, first) = app.announcements.request(&app, "g", "").await.unwrap();
        let (same_id, same) = app.announcements.request(&app, "g", "").await.unwrap();
        assert_eq!(first_id, same_id);
        assert_eq!(commands.recv().await.unwrap()["op"], "announcement");
        assert!(commands.try_recv().is_err());
        app.announcements.expire("g", &first_id).await;
        assert_eq!(first.borrow().as_ref().unwrap()["pending"], true);
        assert_eq!(same.borrow().as_ref().unwrap()["available"], false);
        let (second_id, second) = app.announcements.request(&app, "g", "").await.unwrap();
        let response = json!({"group":"g","request_id":first_id,"available":true,"set":true,"text":"旧公告","author_id":"a","author":"甲","avatar":"","updated_at":123,"path":"private"});
        app.announcements.finish(response.clone()).await;
        assert!(second.borrow().is_none());
        let mut response = response;
        response["request_id"] = json!(second_id);
        app.announcements.finish(response).await;
        let response = second.borrow().clone().unwrap();
        assert_eq!(response["text"], "旧公告");
        assert!(response.get("path").is_none());
        assert!(response.get("request_id").is_none());
        let (_, third) = app.announcements.request(&app, "g", "").await.unwrap();
        app.announcements.fail_all().await;
        assert_eq!(third.borrow().as_ref().unwrap()["available"], false);
        assert_eq!(third.borrow().as_ref().unwrap()["pending"], false);
    }

    #[test]
    fn unavailable_unset_and_malformed_announcement_are_distinct_and_bounded() {
        let unset = json!({"available":true,"set":false,"text":"","author_id":"","author":"","avatar":"","updated_at":null});
        assert_eq!(sanitized(&unset, "g")["available"], true);
        assert_eq!(sanitized(&unset, "g")["set"], false);
        let mut invalid = unset.clone();
        invalid["text"] = json!("x".repeat(65537));
        assert_eq!(sanitized(&invalid, "g")["available"], false);
        invalid = unset.clone();
        invalid["avatar"] = json!("https://private.invalid/");
        assert_eq!(sanitized(&invalid, "g")["available"], false);
        invalid = unset;
        invalid["available"] = json!(false);
        invalid["text"] = json!("must not leak");
        assert_eq!(sanitized(&invalid, "g")["text"], "");
        assert_eq!(sanitized(&invalid, "g")["updated_at"], Value::Null);
    }
}

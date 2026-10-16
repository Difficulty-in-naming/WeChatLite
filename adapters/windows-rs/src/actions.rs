use crate::{native::NativeClient, recall::DefinitelyNotSubmitted};
use anyhow::Result;
use serde_json::Value;
use std::{collections::BTreeSet, path::PathBuf};

pub struct Actions {
    allowed: BTreeSet<String>,
}
impl Actions {
    pub fn new(directory: PathBuf, _wxid: &str) -> Result<Self> {
        // Retire the old native queue before enabling the UI route.
        let mut previous = NativeClient::new(directory.join("native-actions"));
        previous.restrict(&[])?;
        Ok(Self {
            allowed: BTreeSet::new(),
        })
    }
    pub fn select(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        self.allowed = groups.clone();
        Ok(())
    }
    pub fn submit(&mut self, group: &str, info: &Value, id: &str) -> Result<()> {
        if !self.allowed.contains(group)
            || info["group"].as_str() != Some(group)
            || id.len() != 64
            || !id.bytes().all(|byte| byte.is_ascii_hexdigit())
        {
            return Err(anyhow::Error::new(DefinitelyNotSubmitted)
                .context("撤回群未获准或请求信息无效，未操作微信"));
        }
        crate::chat_activation::recall(info)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn stale_authorization_is_cleared_even_before_optional_dll_loading() {
        let temp = tempfile::tempdir().unwrap();
        let old = temp.path().join("native-actions/old/native.json");
        std::fs::create_dir_all(old.parent().unwrap()).unwrap();
        std::fs::write(
            &old,
            json!({"token":"a".repeat(64),"port":1,"groups":["12345@chatroom"]}).to_string(),
        )
        .unwrap();
        let actions = Actions::new(temp.path().to_owned(), "wxid_fixture");
        let config: Value = serde_json::from_slice(&std::fs::read(&old).unwrap()).unwrap();
        assert_eq!(config["groups"], json!([]));
        drop(actions);
    }

    #[test]
    fn selecting_groups_and_rejecting_invalid_requests_never_opens_wechat() {
        let temp = tempfile::tempdir().unwrap();
        let mut actions = Actions::new(temp.path().to_owned(), "wxid_fixture").unwrap();
        let group = "12345@chatroom";
        let info = json!({"group":group});
        let id = "a".repeat(64);
        assert!(
            actions
                .submit(group, &info, &id)
                .unwrap_err()
                .downcast_ref::<DefinitelyNotSubmitted>()
                .is_some()
        );
        actions.select(&BTreeSet::from([group.to_owned()])).unwrap();
        for (target, body, request_id) in [
            ("54321@chatroom", info.clone(), id.as_str()),
            (group, json!({}), id.as_str()),
            (group, info.clone(), "invalid-request"),
        ] {
            assert!(
                actions
                    .submit(target, &body, request_id)
                    .unwrap_err()
                    .downcast_ref::<DefinitelyNotSubmitted>()
                    .is_some()
            );
        }
        actions.select(&BTreeSet::new()).unwrap();
        assert!(
            actions
                .submit(group, &info, &id)
                .unwrap_err()
                .downcast_ref::<DefinitelyNotSubmitted>()
                .is_some()
        );
    }
}

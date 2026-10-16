use crate::{
    engine::{Rejected, text},
    native::{NativeClient, NativeHttpError},
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{collections::BTreeSet, path::PathBuf};

pub struct Actions {
    client: NativeClient,
    root: PathBuf,
    connected: bool,
}
impl Actions {
    pub fn new(directory: PathBuf, wxid: &str) -> Result<Self> {
        // Revoke stale permissions even when the optional DLL is missing or cannot be read.
        let mut previous = NativeClient::new(directory.join("native-actions"));
        previous.restrict(&[])?;
        drop(previous);
        let hash = include_str!("../vendor/WeChatActions.sha256").trim();
        ensure!(
            hash.len() == 64 && hash.bytes().all(|c| c.is_ascii_hexdigit()),
            "撤回组件尚未构建或指纹缺失"
        );
        let executable = std::env::current_exe()?;
        let path = executable
            .parent()
            .context("程序目录缺失")?
            .join("WeChatActions.dll");
        let binary = std::fs::read(path).context("缺少独立撤回组件 WeChatActions.dll")?;
        let mut client = NativeClient::with_binary(
            directory.join("native-actions"),
            binary,
            hash,
            json!({"self_wxid":wxid}),
        );
        client.restrict(&[])?;
        Ok(Self {
            client,
            root: directory,
            connected: false,
        })
    }
    pub fn select(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        let list: Vec<_> = groups.iter().cloned().collect();
        if list.is_empty() {
            self.client.restrict(&[])?;
            if self.connected {
                let _ = self.client.request("/stop", Some(&json!({})));
            }
            self.connected = false;
            return Ok(());
        }
        let health = self
            .client
            .connect(&list, &self.root.join("feishu/media"))?;
        ensure!(
            health["capabilities"]
                .as_array()
                .is_some_and(|list| list.iter().any(|cap| cap == "recall")),
            "当前原生组件不支持撤回"
        );
        let response = self.client.request("/start", Some(&json!({})))?;
        ensure!(response["enabled"] == true, "微信撤回观察未启用");
        self.connected = true;
        Ok(())
    }
    pub fn submit(&mut self, group: &str, info: &Value, id: &str) -> Result<()> {
        ensure!(self.connected, "微信撤回观察未启用");
        let response=self.client.request("/recall",Some(&json!({"group":group,"server_id":info["server_id"],"create_time":info["create_time"],"local_id":info["local_id"],"request_id":id})))
            .map_err(|error|if error.downcast_ref::<NativeHttpError>().is_some_and(NativeHttpError::definite_rejection){anyhow::Error::new(Rejected).context(format!("微信撤回未提交：{error}"))}else{error})?;
        ensure!(
            response["status"] == "queued"
                && response["server_id"] == info["server_id"]
                && response["request_id"] == id,
            "撤回提交结果未获确认，未重复提交"
        );
        Ok(())
    }
    pub fn progress(&mut self, group: &str, id: &str) -> Result<Option<Value>> {
        ensure!(
            group
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"@_-".contains(&c))
                && id.len() == 64
                && id.bytes().all(|c| c.is_ascii_hexdigit()),
            "撤回查询编号无效"
        );
        if !self.connected && !self.client.attach_existing()? {
            return Ok(None);
        }
        match self
            .client
            .request(&format!("/result?request_id={id}&group={group}"), None)
        {
            Ok(value) => {
                ensure!(text(&value, "request_id") == id, "撤回回执编号不匹配");
                Ok(Some(value))
            }
            Err(error)
                if error
                    .downcast_ref::<NativeHttpError>()
                    .is_some_and(|error| error.code == 404) =>
            {
                Ok(None)
            }
            Err(error) => Err(error),
        }
    }
}
impl Drop for Actions {
    fn drop(&mut self) {
        let _ = self.select(&BTreeSet::new());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
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
}

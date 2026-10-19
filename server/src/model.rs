use serde::{Deserialize, Serialize};

pub const RECALL_UNAVAILABLE: &str = "当前微信版本的后台撤回暂不可用；请在原版微信中撤回";

pub fn recall_unavailable(status: &serde_json::Value) -> Option<&str> {
    if status["recall_supported"] != false {
        return None;
    }
    Some(
        status["recall_reason"]
            .as_str()
            .filter(|reason| !reason.is_empty())
            .unwrap_or(RECALL_UNAVAILABLE),
    )
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Group {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub count: usize,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub avatar: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Member {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub remark: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub avatar: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Mention {
    pub id: String,
    pub name: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Media {
    pub id: String,
    pub name: String,
    pub kind: String,
    pub size: u64,
    pub sha256: String,
    #[serde(default)]
    pub path: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Quote {
    pub id: String,
    #[serde(default)]
    pub sender: String,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub unavailable: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ForwardItem {
    pub id: String,
    #[serde(default)]
    pub sender: String,
    #[serde(default)]
    pub text: String,
    pub timestamp: i64,
    #[serde(default)]
    pub media: Vec<usize>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ForwardRecord {
    pub title: String,
    pub items: Vec<ForwardItem>,
    #[serde(default)]
    pub pending: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ChatMessage {
    #[serde(default)]
    pub seq: i64,
    pub id: String,
    pub group: String,
    pub sender: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub avatar: String,
    #[serde(default)]
    pub sender_id: String,
    #[serde(default)]
    pub client_id: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub access_client: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub recall_client: String,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub media: Vec<Media>,
    #[serde(default)]
    pub mentions: Vec<Mention>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub quote: Option<Quote>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub record: Option<ForwardRecord>,
    pub timestamp: i64,
    pub status: String,
    #[serde(default)]
    pub error: String,
    #[serde(default)]
    pub source_id: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub source_ids: Vec<String>,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub recall_status: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub recall_error: String,
    #[serde(default)]
    pub recall_retryable: bool,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub recall_attempt: String,
    #[serde(default)]
    pub outgoing: bool,
}

pub fn clean_name(name: &str) -> String {
    let name = name.rsplit(['/', '\\']).next().unwrap_or("附件");
    let cleaned: String = name
        .chars()
        .filter(|c| !c.is_control())
        .map(|c| if "<>:\"/\\|?*".contains(c) { '_' } else { c })
        .take(160)
        .collect();
    let trimmed = cleaned.trim().trim_end_matches(['.', ' ']);
    if trimmed.is_empty() || trimmed == "." || trimmed == ".." {
        "附件".into()
    } else {
        trimmed.into()
    }
}

pub fn detect_kind(name: &str, header: &[u8]) -> &'static str {
    if header.starts_with(b"GIF87a") || header.starts_with(b"GIF89a") {
        return "gif";
    }
    if header.starts_with(b"\x89PNG\r\n\x1a\n")
        || header.starts_with(b"\xff\xd8\xff")
        || (header.starts_with(b"RIFF") && header.get(8..12) == Some(b"WEBP"))
    {
        return "image";
    }
    let lower = name.to_lowercase();
    if ["mp4", "mov", "m4v", "webm", "mkv", "avi"]
        .iter()
        .any(|ext| lower.ends_with(&format!(".{ext}")))
    {
        return "video";
    }
    if ["mp3", "wav", "ogg", "opus", "m4a", "silk", "amr"]
        .iter()
        .any(|ext| lower.ends_with(&format!(".{ext}")))
    {
        return "voice";
    }
    "file"
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn optional_avatars_preserve_legacy_json_and_roundtrip_when_present() {
        let mut group: Group = serde_json::from_value(json!({"id":"g","name":"群"})).unwrap();
        let mut member: Member = serde_json::from_value(json!({"id":"m","name":"成员"})).unwrap();
        let mut message: ChatMessage = serde_json::from_value(
            json!({"id":"message","group":"g","sender":"成员","timestamp":0,"status":"received"}),
        )
        .unwrap();
        for value in [json!(&group), json!(&member), json!(&message)] {
            assert!(value.get("avatar").is_none());
        }
        let encoded = "c21hbGwtanBlZw==";
        group.avatar = encoded.into();
        member.avatar = encoded.into();
        message.avatar = encoded.into();
        for value in [json!(&group), json!(&member), json!(&message)] {
            assert_eq!(value["avatar"], encoded);
        }
        assert_eq!(
            serde_json::from_value::<Group>(json!(group))
                .unwrap()
                .avatar,
            encoded
        );
        assert_eq!(
            serde_json::from_value::<Member>(json!(member))
                .unwrap()
                .avatar,
            encoded
        );
        assert_eq!(
            serde_json::from_value::<ChatMessage>(json!(message))
                .unwrap()
                .avatar,
            encoded
        );
    }
    #[test]
    fn names_cannot_escape_download_root() {
        assert_eq!(clean_name("../../a\\b:hello?.gif"), "b_hello_.gif");
        assert_eq!(clean_name(".."), "附件");
        assert_eq!(clean_name("a\r\nb.txt"), "ab.txt");
    }
    #[test]
    fn gif_is_recognized_by_content() {
        assert_eq!(detect_kind("a.bin", b"GIF89a0000"), "gif");
        assert_eq!(detect_kind("clip.MP4", b"0000ftyp"), "video");
    }
}

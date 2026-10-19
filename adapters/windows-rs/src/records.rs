//! Merged-forward cards keep their messages and media in the enclosing group's permission scope.
use anyhow::{Context, Result, ensure};
use roxmltree::Node;
use serde_json::{Value, json};

pub struct Attachment {
    pub kind: &'static str,
    pub file_id: String,
    pub aes: String,
    pub length: u64,
    pub md5: String,
    pub name: String,
}

fn child<'a>(node: Node<'a, 'a>, name: &str) -> &'a str {
    node.children()
        .find(|n| n.has_tag_name(name))
        .and_then(|n| n.text())
        .unwrap_or("")
}

pub fn is_record(content: &str) -> bool {
    let Ok(doc) = roxmltree::Document::parse(content) else {
        return false;
    };
    doc.descendants()
        .find(|n| n.has_tag_name("appmsg"))
        .is_some_and(|n| child(n, "type") == "19")
}

pub fn parse(
    content: &str,
    key: &str,
    fallback_time: i64,
    mut download: impl FnMut(&str, &Attachment) -> Result<Value>,
) -> Result<(Value, Vec<Value>)> {
    ensure!(content.len() <= 1024 * 1024, "合并聊天记录过大");
    let doc = roxmltree::Document::parse(content)?;
    let app = doc
        .descendants()
        .find(|n| n.has_tag_name("appmsg"))
        .context("缺少聊天记录卡片")?;
    ensure!(child(app, "type") == "19", "不是合并聊天记录");
    let record =
        roxmltree::Document::parse(child(app, "recorditem")).context("聊天记录内容无法解析")?;
    let mut items = vec![];
    let mut media = vec![];
    let mut pending = false;
    visit(
        record.root_element(),
        key,
        fallback_time,
        0,
        &mut items,
        &mut media,
        &mut pending,
        &mut download,
    )?;
    ensure!(!items.is_empty(), "聊天记录没有可读取的消息");
    let title = child(app, "title");
    Ok((
        json!({"title":if title.is_empty(){"聊天记录"}else{title},"items":items,"pending":pending}),
        media,
    ))
}

fn visit(
    record: Node<'_, '_>,
    key: &str,
    fallback_time: i64,
    depth: usize,
    items: &mut Vec<Value>,
    media: &mut Vec<Value>,
    pending: &mut bool,
    download: &mut impl FnMut(&str, &Attachment) -> Result<Value>,
) -> Result<()> {
    ensure!(depth <= 6, "嵌套聊天记录超过 6 层");
    let list = record
        .children()
        .find(|n| n.has_tag_name("datalist"))
        .context("聊天记录缺少消息列表")?;
    for (index, item) in list
        .children()
        .filter(|n| n.has_tag_name("dataitem"))
        .enumerate()
    {
        ensure!(items.len() < 200, "合并聊天记录超过 200 条");
        let id = format!("{key}/record/{index}");
        let timestamp = child(item, "srcMsgCreateTime")
            .parse::<i64>()
            .ok()
            .filter(|t| *t > 0 && *t < 32_503_680_000)
            .map(|t| t * 1000)
            .unwrap_or(fallback_time);
        let kind = match item.attribute("datatype").unwrap_or("") {
            "2" => Some("image"),
            "3" => Some("voice"),
            "4" => Some("video"),
            "8" => Some("file"),
            _ => None,
        };
        let mut text = child(item, "datadesc").to_owned();
        let mut attachments = vec![];
        let mut waiting = false;
        if let Some(kind) = kind {
            let mut name = child(item, "datatitle").to_owned();
            let ext = match (child(item, "datafmt"), kind) {
                ("", "video") => "mp4",
                ("", "voice") => "silk",
                (ext, _) => ext,
            };
            if name.is_empty() {
                name = match kind {
                    "image" => "图片",
                    "voice" => "语音",
                    "video" => "视频",
                    _ => "附件",
                }
                .into();
            }
            if !ext.is_empty()
                && ext.len() <= 12
                && ext.bytes().all(|c| c.is_ascii_alphanumeric())
                && !name
                    .to_lowercase()
                    .ends_with(&format!(".{}", ext.to_lowercase()))
            {
                name.push('.');
                name.push_str(ext);
            }
            let resource = Attachment {
                kind,
                file_id: child(item, "cdndataurl").into(),
                aes: child(item, "cdndatakey").into(),
                length: child(item, "datasize").parse().unwrap_or(0),
                md5: child(item, "fullmd5").to_ascii_lowercase(),
                name,
            };
            match download(&id, &resource) {
                Ok(value) => {
                    attachments.push(media.len());
                    media.push(value);
                }
                Err(error) => {
                    waiting = error.is::<crate::media::MediaPending>();
                    let label = match kind {
                        "image" => "图片",
                        "voice" => "语音",
                        "video" => "视频",
                        _ => "附件",
                    };
                    text = format!(
                        "[{label}] {}{}",
                        resource.name,
                        if waiting {
                            " · 后台下载中，会自动重试"
                        } else {
                            " · 暂时无法读取"
                        }
                    );
                }
            }
        } else if item.attribute("datatype") != Some("1") {
            if let Some(nested) = item.children().find(|n| n.has_tag_name("recordinfo")) {
                visit(
                    nested,
                    &id,
                    timestamp,
                    depth + 1,
                    items,
                    media,
                    pending,
                    download,
                )?;
                continue;
            }
            text = [
                child(item, "datatitle"),
                child(item, "datadesc"),
                child(item, "weburl"),
            ]
            .into_iter()
            .filter(|s| !s.is_empty())
            .collect::<Vec<_>>()
            .join("\n");
            if text.is_empty() {
                text = "[暂不支持的记录消息]".into();
            }
        }
        *pending |= waiting;
        items.push(json!({"id":id,"sender":child(item,"sourcename"),"timestamp":timestamp,"text":text,"media":attachments}));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn preserves_order_text_and_full_resources_without_exposing_cdn_credentials() {
        let xml = r#"<msg><appmsg><title>聊天记录</title><type>19</type><recorditem><![CDATA[<recordinfo><datalist count="3"><dataitem datatype="2"><sourcename>A</sourcename><cdndataurl>full-resource</cdndataurl><cdnthumburl>thumbnail</cdnthumburl><cdndatakey>secret</cdndatakey><datasize>123</datasize><fullmd5>digest</fullmd5><datafmt>jpg</datafmt></dataitem><dataitem datatype="1"><sourcename>B</sourcename><datadesc>&lt;hello&gt;</datadesc><srcMsgCreateTime>1760000000</srcMsgCreateTime></dataitem><dataitem datatype="2"><sourcename>A</sourcename></dataitem></datalist></recordinfo>]]></recorditem></appmsg></msg>"#;
        let mut calls = 0;
        let (record, media) = parse(xml, "source", 1, |_, resource| {
            calls += 1;
            if calls == 1 {
                assert_eq!(resource.file_id, "full-resource");
                assert_eq!(resource.length, 123);
                Ok(json!({"kind":"image","size":123,"sha256":"verified","path":"private"}))
            } else {
                Err(crate::media::MediaPending("waiting".into()).into())
            }
        })
        .unwrap();
        assert_eq!(media.len(), 1);
        assert_eq!(record["items"][0]["media"], json!([0]));
        assert_eq!(record["items"][1]["text"], "<hello>");
        assert_eq!(record["items"][1]["timestamp"], 1760000000000i64);
        assert_eq!(record["pending"], true);
        assert!(!record.to_string().contains("secret"));
        assert!(!record.to_string().contains("thumbnail"));
    }
}

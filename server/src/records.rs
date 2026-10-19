use crate::{history::Progress, model::ChatMessage, store::Store};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const SCAN_LIMIT: usize = 200;

#[derive(Clone, Deserialize, Serialize)]
pub struct Query {
    pub group: String,
    #[serde(default = "default_limit")]
    pub limit: usize,
    #[serde(default)]
    pub before: String,
    #[serde(default)]
    pub q: String,
    #[serde(default = "default_kind", rename = "type")]
    pub kind: String,
    pub start: Option<i64>,
    pub end: Option<i64>,
    #[serde(default)]
    pub member: String,
}
fn default_limit() -> usize {
    50
}
fn default_kind() -> String {
    "all".into()
}

impl Query {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            (1..=50).contains(&self.limit),
            "记录页大小需要在 1 至 50 之间"
        );
        ensure!(
            self.q.len() <= 512 && !self.q.contains('\0'),
            "搜索词无效或过长"
        );
        ensure!(
            self.member.len() <= 1024 && !self.member.chars().any(char::is_control),
            "成员筛选无效"
        );
        ensure!(
            ["all", "file", "media", "link", "voice"].contains(&self.kind.as_str()),
            "记录类型无效"
        );
        ensure!(
            self.start.is_none_or(|n| n >= 0) && self.end.is_none_or(|n| n >= 0),
            "日期范围无效"
        );
        ensure!(
            self.start
                .zip(self.end)
                .is_none_or(|(start, end)| start < end),
            "日期范围无效"
        );
        Ok(())
    }
    fn fingerprint(&self) -> String {
        let fields = (
            &self.group,
            &self.q,
            &self.kind,
            self.start,
            self.end,
            &self.member,
        );
        format!("{:x}", Sha256::digest(serde_json::to_vec(&fields).unwrap()))
    }
    pub fn matches(&self, row: &ChatMessage) -> bool {
        row.status != "recalled"
            && (self.member.is_empty() || row.sender_id == self.member)
            && self.start.is_none_or(|n| row.timestamp >= n)
            && self.end.is_none_or(|n| row.timestamp < n)
            && (self.q.is_empty()
                || row.text.contains(&self.q)
                || row.sender.contains(&self.q)
                || row.record.as_ref().is_some_and(|record| {
                    record
                        .items
                        .iter()
                        .any(|item| item.text.contains(&self.q) || item.sender.contains(&self.q))
                })
                || row.media.iter().any(|m| m.name.contains(&self.q)))
            && match self.kind.as_str() {
                "file" => row.media.iter().any(|m| m.kind == "file"),
                "media" => row
                    .media
                    .iter()
                    .any(|m| ["image", "video", "gif"].contains(&m.kind.as_str())),
                "voice" => row.media.iter().any(|m| m.kind == "voice"),
                "link" => ["https://", "http://", "www."]
                    .iter()
                    .any(|s| row.text.contains(s)),
                _ => true,
            }
    }
}

/// The immutable initial snapshot and later historical imports have independent keysets.
/// Inserting an old row with a newer relay seq must not skip a same-second row or a page tail.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Cursor {
    fingerprint: String,
    pub snapshot: i64,
    pub base_before: Option<(i64, i64)>,
    pub base_done: bool,
    pub high: i64,
    pub extra_high: i64,
    pub extra_before: Option<(i64, i64)>,
    pub native: String,
}
impl Cursor {
    pub fn load(query: &Query, snapshot: i64, progress: &Progress) -> Result<Self> {
        if query.before.is_empty() {
            return Ok(Self {
                fingerprint: query.fingerprint(),
                snapshot,
                base_before: None,
                base_done: snapshot == 0,
                high: snapshot,
                extra_high: 0,
                extra_before: None,
                native: marker(progress),
            });
        }
        ensure!(
            query.before.len() <= 2048 && query.before.len().is_multiple_of(2),
            "记录游标无效"
        );
        let bytes = query
            .before
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| {
                let pair = std::str::from_utf8(pair)?;
                Ok(u8::from_str_radix(pair, 16)?)
            })
            .collect::<Result<Vec<_>>>()?;
        let result: Self = serde_json::from_slice(&bytes).context("记录游标无效")?;
        ensure!(
            result.fingerprint == query.fingerprint(),
            "记录游标与群或筛选条件不一致"
        );
        ensure!(
            result.snapshot >= 0
                && result.high >= result.snapshot
                && result.high <= snapshot
                && (result.extra_high == 0
                    || (result.extra_high > result.high && result.extra_high <= snapshot))
                && result
                    .base_before
                    .is_none_or(|(_, seq)| seq > 0 && seq <= result.snapshot)
                && result
                    .extra_before
                    .is_none_or(|(_, seq)| seq > result.high && seq <= result.extra_high)
                && (result.extra_high != 0 || result.extra_before.is_none())
                && result.native.len() == 64,
            "记录游标已失效"
        );
        Ok(result)
    }
    pub fn encode(&self) -> String {
        serde_json::to_vec(self)
            .unwrap()
            .iter()
            .map(|n| format!("{n:02x}"))
            .collect()
    }
    pub fn set_progress(&mut self, progress: &Progress) {
        self.native = marker(progress);
    }
}
fn marker(progress: &Progress) -> String {
    format!(
        "{:x}",
        Sha256::digest(
            serde_json::to_vec(&(
                progress.initialized,
                &progress.before_source,
                progress.exhausted
            ))
            .unwrap()
        )
    )
}

pub struct Scan {
    pub messages: Vec<ChatMessage>,
    pub scanned: usize,
    pub cached_more: bool,
}

pub fn scan(store: &Store, query: &Query, cursor: &mut Cursor, budget: usize) -> Result<Scan> {
    let mut result = Scan {
        messages: Vec::new(),
        scanned: 0,
        cached_more: false,
    };
    loop {
        if cursor.extra_high == 0 {
            let head = store.record_import_head(&query.group)?;
            if head > cursor.high {
                cursor.extra_high = head;
            }
        }
        let extra = cursor.extra_high > 0;
        if !extra && cursor.base_done {
            return Ok(result);
        }
        if result.scanned >= budget || result.messages.len() >= query.limit {
            result.cached_more = true;
            return Ok(result);
        }
        let rows = store.record_rows(
            &query.group,
            if extra { cursor.high } else { 0 },
            if extra {
                cursor.extra_high
            } else {
                cursor.snapshot
            },
            if extra {
                cursor.extra_before
            } else {
                cursor.base_before
            },
            extra,
            budget - result.scanned + 1,
        )?;
        let mut consumed = 0;
        for row in &rows {
            if result.scanned >= budget || result.messages.len() >= query.limit {
                break;
            }
            let position = Some((row.timestamp, row.seq));
            if extra {
                cursor.extra_before = position;
            } else {
                cursor.base_before = position;
            }
            result.scanned += 1;
            consumed += 1;
            if query.matches(row) {
                result.messages.push(row.clone());
            }
        }
        if consumed < rows.len() {
            result.cached_more = true;
            return Ok(result);
        }
        if extra {
            cursor.high = cursor.extra_high;
            cursor.extra_high = 0;
            cursor.extra_before = None;
        } else {
            cursor.base_done = true;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::collections::BTreeSet;

    fn query() -> Query {
        serde_json::from_value(json!({"group":"g"})).unwrap()
    }
    fn message(index: i64) -> ChatMessage {
        serde_json::from_value(json!({"id":format!("row-{index}"),"group":"g","sender":"成员","sender_id":"actor","text":"普通消息","timestamp":1000,"status":"received"})).unwrap()
    }
    #[test]
    fn empty_filtered_pages_advance_bounded_scan_and_bind_cursor_to_filters_and_group() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("rows.sqlite")).unwrap();
        for index in 1..=260 {
            let mut row = message(index);
            if index == 1 {
                row.text = "更早的目标".into();
            }
            store.insert_message(&mut row).unwrap();
        }
        let mut q = query();
        q.q = "目标".into();
        let progress = Progress::default();
        let mut cursor = Cursor::load(&q, 260, &progress).unwrap();
        let first = scan(&store, &q, &mut cursor, SCAN_LIMIT).unwrap();
        assert!(first.messages.is_empty());
        assert_eq!(first.scanned, 200);
        assert!(first.cached_more);
        q.before = cursor.encode();
        assert!(q.before.len() < 2048);
        let mut cursor = Cursor::load(&q, 260, &progress).unwrap();
        let second = scan(&store, &q, &mut cursor, SCAN_LIMIT).unwrap();
        assert_eq!(second.messages.len(), 1);
        assert_eq!(second.messages[0].id, "row-1");
        assert_eq!(second.scanned, 60);
        assert!(!second.cached_more);
        assert_ne!(q.before, cursor.encode());
        let mut changed = q.clone();
        changed.group = "other".into();
        assert!(Cursor::load(&changed, 260, &progress).is_err());
        changed = q.clone();
        changed.q = "other".into();
        assert!(Cursor::load(&changed, 260, &progress).is_err());
        changed = q.clone();
        changed.before = "invalid".into();
        assert!(Cursor::load(&changed, 260, &progress).is_err());
        changed = q;
        changed.before = cursor.encode();
        changed.limit = 20;
        assert!(Cursor::load(&changed, 260, &progress).is_ok());
    }

    #[test]
    fn late_same_second_history_and_delayed_media_have_independent_pagination_without_realtime_duplicates()
     {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("rows.sqlite");
        let store = Store::open(&path).unwrap();
        for index in 1..=70 {
            store.insert_message(&mut message(index)).unwrap();
        }
        let mut q = query();
        let progress = Progress {
            exhausted: true,
            ..Default::default()
        };
        let mut cursor = Cursor::load(&q, 70, &progress).unwrap();
        let first = scan(&store, &q, &mut cursor, SCAN_LIMIT).unwrap();
        let mut ids = first
            .messages
            .into_iter()
            .map(|m| m.id)
            .collect::<BTreeSet<_>>();
        assert_eq!(ids.len(), 50);
        for index in 71..=190 {
            store
                .insert_historical_message(&mut message(index))
                .unwrap();
        }
        store.insert_message(&mut message(191)).unwrap();
        loop {
            q.before = cursor.encode();
            cursor = Cursor::load(&q, 191, &progress).unwrap();
            let page = scan(&store, &q, &mut cursor, SCAN_LIMIT).unwrap();
            assert!(page.scanned <= SCAN_LIMIT);
            for row in page.messages {
                assert!(ids.insert(row.id), "duplicate row");
            }
            if !page.cached_more {
                break;
            }
        }
        assert_eq!(ids.len(), 190);
        assert!(!ids.contains("row-191"));
        q.before = cursor.encode();
        drop(store);
        let store = Store::open(&path).unwrap();
        store.insert_historical_message(&mut message(192)).unwrap();
        let mut cursor = Cursor::load(&q, 192, &progress).unwrap();
        let late = scan(&store, &q, &mut cursor, SCAN_LIMIT).unwrap();
        assert_eq!(late.messages.len(), 1);
        assert_eq!(late.messages[0].id, "row-192");
        assert!(!late.cached_more);
    }

    #[test]
    fn filters_use_visible_fields_dates_member_and_actual_media_kinds_and_never_recalled_text() {
        let mut row = message(1);
        row.media=serde_json::from_value(json!([{"id":"hidden-id","name":"报告.pdf","kind":"file","path":"private-hidden-path","sha256":"hidden-sha","size":1}])).unwrap();
        let mut q = query();
        for needle in ["报告", "成员", "普通"] {
            q.q = needle.into();
            assert!(q.matches(&row));
        }
        for hidden in [
            "hidden-id",
            "private-hidden-path",
            "hidden-sha",
            "actor",
            "row-1",
        ] {
            q.q = hidden.into();
            assert!(!q.matches(&row));
        }
        q.q.clear();
        q.kind = "file".into();
        q.member = "actor".into();
        q.start = Some(1000);
        q.end = Some(1001);
        assert!(q.matches(&row));
        q.end = Some(1000);
        assert!(!q.matches(&row));
        q.end = Some(1001);
        q.member = "other".into();
        assert!(!q.matches(&row));
        q.member.clear();
        q.kind = "media".into();
        assert!(!q.matches(&row));
        for kind in ["image", "video", "gif"] {
            row.media[0].kind = kind.into();
            assert!(q.matches(&row));
        }
        q.kind = "voice".into();
        row.media[0].kind = "voice".into();
        assert!(q.matches(&row));
        q.kind = "link".into();
        assert!(!q.matches(&row));
        row.text = "https://example.invalid/".into();
        assert!(q.matches(&row));
        row.status = "recalled".into();
        assert!(!q.matches(&row));
    }

    #[test]
    fn foreign_anchor_is_rejected_even_with_a_forged_but_filter_bound_cursor() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("rows.sqlite")).unwrap();
        let mut foreign = message(1);
        foreign.group = "other".into();
        store.insert_message(&mut foreign).unwrap();
        store.insert_message(&mut message(2)).unwrap();
        let q = query();
        let mut cursor = Cursor::load(&q, 2, &Progress::default()).unwrap();
        cursor.base_before = Some((foreign.timestamp, foreign.seq));
        assert!(scan(&store, &q, &mut cursor, SCAN_LIMIT).is_err());
    }
}

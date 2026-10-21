use crate::model::Group;
use anyhow::{Result, ensure};
use std::collections::BTreeSet;

pub fn validate(patterns: &BTreeSet<String>) -> Result<()> {
    ensure!(patterns.len() <= 32, "群名规则最多 32 条");
    for pattern in patterns {
        ensure!(
            !pattern.is_empty()
                && pattern.trim() == pattern
                && pattern.chars().count() <= 256
                && !pattern.chars().any(char::is_control),
            "群名规则需要 1 至 256 个可显示字符，首尾不能有空格"
        );
    }
    Ok(())
}

// Anchored, case-insensitive globs: only * and ? have special meaning.
fn matches(pattern: &str, name: &str) -> bool {
    let name: Vec<char> = name.trim().to_lowercase().chars().collect();
    let mut previous = vec![false; name.len() + 1];
    previous[0] = true;
    for ch in pattern.to_lowercase().chars() {
        let mut next = vec![false; name.len() + 1];
        if ch == '*' {
            next[0] = previous[0];
            for i in 1..=name.len() {
                next[i] = previous[i] || next[i - 1];
            }
        } else {
            for i in 1..=name.len() {
                next[i] = previous[i - 1] && (ch == '?' || ch == name[i - 1]);
            }
        }
        previous = next;
    }
    previous[name.len()]
}

pub fn resolve(patterns: &BTreeSet<String>, groups: &[Group]) -> BTreeSet<String> {
    groups
        .iter()
        .filter(|group| patterns.iter().any(|pattern| matches(pattern, &group.name)))
        .map(|group| group.id.clone())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn globs_match_whole_group_names_without_regex_or_path_semantics() {
        for name in ["AA-PLY001-X - jx", "BB-PLY002 - jx", "cc-ply003 - JX  "] {
            assert!(matches("*-PLY* - jx", name), "{name}");
        }
        for name in [
            "BB-PLY002 - other",
            "BB-ABC002 - jx",
            "BB-PLY002 - jx extra",
        ] {
            assert!(!matches("*-PLY* - jx", name), "{name}");
        }
        assert!(matches("群? [A]", "群聊 [A]"));
        assert!(!matches("群? [A]", "群聊 A"));
        assert!(!matches("群?", "群聊天"));
        assert!(matches("a**?c*", "abcc"));
        assert!(matches("*", ""));
        for invalid in ["", " *", "* ", "a\nb"] {
            assert!(validate(&BTreeSet::from([invalid.into()])).is_err());
        }
        assert!(validate(&(0..33).map(|i| format!("group-{i}*")).collect()).is_err());
    }
}

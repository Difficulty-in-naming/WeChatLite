use anyhow::Result;
use rusqlite::{Connection, types::ValueRef};
use serde_json::{Value, json};

const MAX_TEXT_BYTES: usize = 64 * 1024;

fn unavailable() -> Value {
    json!({"available":false,"set":false,"author_id":"","author":"","avatar":"","updated_at":null,"text":"","error":"unavailable"})
}

fn utf8(value: ValueRef<'_>, limit: usize) -> Option<&str> {
    let bytes = match value {
        ValueRef::Text(bytes) | ValueRef::Blob(bytes) if bytes.len() <= limit => bytes,
        _ => return None,
    };
    let text = std::str::from_utf8(bytes).ok()?;
    (!text.contains('\0')).then_some(text)
}

fn timestamp(value: ValueRef<'_>) -> Option<i64> {
    let raw = match value {
        ValueRef::Integer(value) => value,
        other => utf8(other, 32)?.parse().ok()?,
    };
    if raw <= 0 {
        return None;
    }
    let millis = if raw < 100_000_000_000 {
        raw.checked_mul(1000)?
    } else {
        raw
    };
    // Unknown or corrupt timestamps must not turn into the current time or overflow the client.
    (millis <= 253_402_300_799_999).then_some(millis)
}

pub(super) fn read(connection: &Connection, group: &str) -> Result<Value> {
    // Missing schema/row/NULL is unknown. An actual empty UTF-8 value is the only unset state.
    let Ok(mut statement) = connection.prepare(
        "SELECT announcement_,announcement_editor_,announcement_publish_time_ \
         FROM chat_room_info_detail WHERE username_=?1 LIMIT 2",
    ) else {
        return Ok(unavailable());
    };
    let mut rows = statement.query([group])?;
    let Some(row) = rows.next()? else {
        return Ok(unavailable());
    };
    let Some(text) = utf8(row.get_ref(0)?, MAX_TEXT_BYTES) else {
        return Ok(unavailable());
    };
    let editor = utf8(row.get_ref(1)?, 1024)
        .filter(|value| !value.chars().any(char::is_control))
        .unwrap_or_default();
    let value = json!({"available":true,"set":!text.is_empty(),"author_id":editor,"author":"","avatar":"","updated_at":timestamp(row.get_ref(2)?),"text":text});
    if rows.next()?.is_some() {
        return Ok(unavailable());
    }
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::params;

    fn fixture() -> Connection {
        let connection = Connection::open_in_memory().unwrap();
        connection.execute_batch("CREATE TABLE chat_room_info_detail(username_ TEXT,announcement_,announcement_editor_,announcement_publish_time_);PRAGMA query_only=ON;").unwrap();
        connection
    }

    fn insert(connection: &Connection, group: &str, text: &[u8], editor: &[u8], time: i64) {
        connection.execute_batch("PRAGMA query_only=OFF").unwrap();
        connection
            .execute(
                "INSERT INTO chat_room_info_detail VALUES(?1,?2,?3,?4)",
                params![group, text, editor, time],
            )
            .unwrap();
        connection.execute_batch("PRAGMA query_only=ON").unwrap();
    }

    #[test]
    fn utf8_blob_metadata_and_empty_text_are_distinct_from_unavailable() {
        let connection = fixture();
        insert(
            &connection,
            "g@chatroom",
            "真实格式\n<原样文字> & 😀".as_bytes(),
            b"fixture-editor",
            1_760_000_000,
        );
        let value = read(&connection, "g@chatroom").unwrap();
        assert_eq!(value["available"], true);
        assert_eq!(value["set"], true);
        assert_eq!(value["text"], "真实格式\n<原样文字> & 😀");
        assert_eq!(value["author_id"], "fixture-editor");
        assert_eq!(value["updated_at"], 1_760_000_000_000_i64);
        assert_eq!(
            read(&connection, "missing@chatroom").unwrap(),
            unavailable()
        );
        connection.execute_batch("PRAGMA query_only=OFF;INSERT INTO chat_room_info_detail VALUES('empty@chatroom','',NULL,0),('null@chatroom',NULL,'',0);PRAGMA query_only=ON;").unwrap();
        let empty = read(&connection, "empty@chatroom").unwrap();
        assert_eq!(empty["available"], true);
        assert_eq!(empty["set"], false);
        assert!(empty["updated_at"].is_null());
        assert_eq!(read(&connection, "null@chatroom").unwrap(), unavailable());
        assert_eq!(
            connection
                .query_row("PRAGMA query_only", [], |row| row.get::<_, i64>(0))
                .unwrap(),
            1
        );
        assert!(
            connection
                .execute("DELETE FROM chat_room_info_detail", [])
                .is_err()
        );
    }

    #[test]
    fn absent_schema_invalid_utf8_oversize_and_duplicate_rows_are_unavailable() {
        let connection = Connection::open_in_memory().unwrap();
        assert_eq!(read(&connection, "g@chatroom").unwrap(), unavailable());
        connection
            .execute_batch("CREATE TABLE chat_room_info_detail(username_ TEXT,announcement_ TEXT)")
            .unwrap();
        assert_eq!(read(&connection, "g@chatroom").unwrap(), unavailable());
        let connection = fixture();
        for (group, bytes) in [
            ("bad@chatroom", vec![0xff]),
            ("big@chatroom", vec![b'x'; MAX_TEXT_BYTES + 1]),
            ("nul@chatroom", b"a\0b".to_vec()),
        ] {
            insert(&connection, group, &bytes, b"", 0);
            assert_eq!(read(&connection, group).unwrap(), unavailable());
        }
        insert(&connection, "duplicate@chatroom", b"one", b"", 0);
        insert(&connection, "duplicate@chatroom", b"two", b"", 0);
        assert_eq!(
            read(&connection, "duplicate@chatroom").unwrap(),
            unavailable()
        );
        assert_eq!(read(&connection, "' OR 1=1 --").unwrap(), unavailable());
    }

    #[test]
    fn timestamps_never_invent_an_update_or_multiply_milliseconds_twice() {
        assert_eq!(
            timestamp(ValueRef::Integer(1_760_000_000)),
            Some(1_760_000_000_000)
        );
        assert_eq!(
            timestamp(ValueRef::Integer(1_760_000_000_123)),
            Some(1_760_000_000_123)
        );
        assert_eq!(
            timestamp(ValueRef::Blob(b"1760000000")),
            Some(1_760_000_000_000)
        );
        for value in [
            ValueRef::Null,
            ValueRef::Integer(0),
            ValueRef::Integer(-1),
            ValueRef::Integer(i64::MAX),
            ValueRef::Text(b"unknown"),
        ] {
            assert_eq!(timestamp(value), None);
        }
    }
}

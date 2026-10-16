//! Explicit text recall through the verified local Weixin UI. No work on startup/polling.
use anyhow::{Context, Result, anyhow, ensure};
use serde_json::Value;

fn not_submitted(message: impl std::fmt::Display) -> anyhow::Error {
    anyhow::Error::new(crate::recall::DefinitelyNotSubmitted).context(message.to_string())
}

struct Input {
    group: Vec<u16>,
    text: Vec<u16>,
    created: i64,
    context: Vec<(Vec<u16>, bool, bool)>,
    unique_contexts: Vec<(usize, usize)>,
    history_gate: Value,
    account: Account,
}
struct Account {
    pid: u32,
    module_base: usize,
    wxid: Vec<u8>,
    roots: Vec<[usize; 4]>,
}
fn string(value: &Value, maximum: usize) -> Result<Vec<u16>> {
    let value = value.as_str().context("原生撤回定位文字缺失")?;
    ensure!(
        !value.is_empty() && !value.contains('\0'),
        "原生撤回定位文字无效"
    );
    let wide: Vec<_> = value.encode_utf16().collect();
    ensure!(wide.len() <= maximum, "原生撤回定位文字过长");
    Ok(wide)
}
fn input(info: &Value, now: i64) -> Result<Input> {
    ensure!(
        info["forbidden_to_retry"] != true,
        "这条旧消息只允许查询，未重复撤回"
    );
    let group = info["group_name"]
        .as_str()
        .context("群名不唯一或未提供，无法安全打开原生微信")?;
    ensure!(
        !group.is_empty() && group.trim() == group && !group.contains(['\r', '\n']),
        "群名无法精确定位"
    );
    let group = string(&info["group_name"], 256)?;
    let text = string(&info["ui_text"], 65536)?;
    let created = info["create_time"].as_i64().context("撤回时间无效")?;
    ensure!(
        created > 0 && now >= created && now - created < 180,
        "已超过3分钟撤回时限"
    );
    let server = info["server_id"].as_str().context("撤回缺少微信消息编号")?;
    ensure!(
        !server.is_empty()
            && server.len() <= 20
            && server.bytes().all(|x| x.is_ascii_digit())
            && server.parse::<u64>()? > 0,
        "微信消息编号无效"
    );
    ensure!(
        info["local_id"].as_i64().is_some_and(|x| x > 0),
        "撤回缺少本地定位编号"
    );
    let rows = info["ui_context"]
        .as_array()
        .context("撤回缺少原生文本上下文")?;
    ensure!(
        (1..=200).contains(&rows.len()),
        "可见文本不足以唯一定位，请先在微信中显示相邻消息"
    );
    let mut context = Vec::with_capacity(rows.len());
    let mut targets = 0;
    let mut target_index = 0;
    for row in rows {
        let value = string(&row["text"], 65536)?;
        let outgoing = row["outgoing"].as_bool().context("文本上下文发送方无效")?;
        let target = row["target"].as_bool().context("文本上下文目标无效")?;
        if target {
            ensure!(outgoing && value == text, "撤回上下文与本人目标消息不一致");
            targets += 1;
            target_index = context.len();
        }
        context.push((value, outgoing, target));
    }
    ensure!(targets == 1, "撤回上下文目标不唯一");
    // Access proves each window occurs exactly once in the group's complete
    // readable history. The UI must still find it uniquely in the native view.
    let proofs = info["ui_unique_contexts"]
        .as_array()
        .context("撤回缺少全群历史唯一性校验")?;
    ensure!((1..=6).contains(&proofs.len()), "撤回唯一性校验无效");
    let mut unique_contexts = Vec::with_capacity(proofs.len());
    for proof in proofs {
        let start = proof["start"]
            .as_u64()
            .and_then(|x| usize::try_from(x).ok())
            .context("撤回唯一文本位置无效")?;
        let length = proof["length"]
            .as_u64()
            .and_then(|x| usize::try_from(x).ok())
            .context("撤回唯一文本长度无效")?;
        ensure!(
            (1..=3).contains(&length)
                && length <= context.len()
                && start <= context.len() - length
                && start <= target_index
                && target_index - start < length
                && !unique_contexts.contains(&(start, length)),
            "撤回唯一文本范围无效"
        );
        ensure!(
            rows[start + 1..start + length]
                .iter()
                .all(|row| row.get("break_before").is_none_or(|value| value == false)),
            "撤回唯一文本范围跨过无法验证的消息"
        );
        unique_contexts.push((start, length));
    }
    let history_gate = info["ui_history_gate"]
        .as_object()
        .filter(|gate| !gate.is_empty())
        .context("撤回缺少完整历史状态校验")?;
    let account = &info["ui_account_gate"];
    let pid = account["pid"]
        .as_u64()
        .and_then(|x| u32::try_from(x).ok())
        .filter(|x| *x > 0)
        .context("微信账号进程定位无效")?;
    let module_base = account["module_base"]
        .as_u64()
        .and_then(|x| usize::try_from(x).ok())
        .filter(|x| *x > 0)
        .context("微信账号模块定位无效")?;
    let wxid = account["wxid"]
        .as_str()
        .filter(|x| !x.is_empty() && x.len() <= 127 && !x.contains('\0'))
        .context("微信账号定位无效")?
        .as_bytes()
        .to_vec();
    let raw_roots = account["roots"].as_array().context("微信账号定位根缺失")?;
    ensure!((1..=16).contains(&raw_roots.len()), "微信账号定位根无效");
    let roots = raw_roots
        .iter()
        .map(|root| {
            let mut fields = [0; 4];
            for (i, key) in ["landmark", "root_slot", "root", "config"]
                .iter()
                .enumerate()
            {
                fields[i] = root[key]
                    .as_u64()
                    .and_then(|x| usize::try_from(x).ok())
                    .filter(|x| *x > 0)
                    .context("微信账号定位地址无效")?;
            }
            Ok(fields)
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(Input {
        group,
        text,
        created,
        context,
        unique_contexts,
        history_gate: Value::Object(history_gate.clone()),
        account: Account {
            pid,
            module_base,
            wxid,
            roots,
        },
    })
}

/// Called once only by the explicit submit path. Success means that the native
/// menu Invoke returned; fresh DB revokemsg confirmation remains the authority.
pub fn recall(info: &Value) -> Result<()> {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| not_submitted("系统时间无效"))?
        .as_secs() as i64;
    let input = input(info, now).map_err(not_submitted)?;
    #[cfg(windows)]
    {
        let (pid, base) = crate::native::verified_ui_process().map_err(not_submitted)?;
        if input.account.pid != pid || input.account.module_base != base {
            return Err(not_submitted("微信登录进程已变化，未执行撤回"));
        }
        return std::thread::Builder::new()
            .name("wechat-recall-ui".into())
            .spawn(move || native::recall(pid, base, input))
            .map_err(|_| not_submitted("无法启动本机撤回操作"))?
            .join()
            .map_err(|_| anyhow!("原生撤回操作意外结束，结果待微信数据库确认"))?;
    }
    #[cfg(not(windows))]
    {
        let _ = input;
        Err(not_submitted("此原生文本撤回路径仅支持 Windows"))
    }
}

#[cfg(windows)]
mod native {
    use super::*;
    use std::ffi::c_void;
    #[repr(C)]
    struct Text {
        text: *const u16,
        length: usize,
        outgoing: u32,
        target: u32,
    }
    #[repr(C)]
    struct AccountRoot {
        landmark: usize,
        root_slot: usize,
        root: usize,
        config: usize,
    }
    #[repr(C)]
    struct UniqueContext {
        start: usize,
        length: usize,
    }
    #[repr(C)]
    struct AccountGate {
        pid: u32,
        module_base: usize,
        wxid: *const u8,
        wxid_length: usize,
        roots: *const AccountRoot,
        root_count: usize,
    }
    #[repr(C)]
    struct Request {
        pid: u32,
        base: usize,
        group: *const u16,
        group_length: usize,
        text: *const u16,
        text_length: usize,
        create_time: i64,
        context: *const Text,
        context_length: usize,
        unique_contexts: *const UniqueContext,
        unique_context_count: usize,
        history_context: *const c_void,
        validate_history: Option<unsafe extern "C" fn(*const c_void) -> u32>,
        account: AccountGate,
    }
    unsafe extern "C" {
        fn wl_chat_ui_recall(request: *const Request, detail: *mut u32) -> u32;
        #[cfg(test)]
        fn wl_chat_ui_selftest() -> u32;
    }
    fn history_result(validate: impl FnOnce() -> Result<()>) -> u32 {
        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(validate)) {
            Ok(Ok(())) => 0,
            _ => 1,
        }
    }
    unsafe extern "C" fn validate_history(context: *const c_void) -> u32 {
        if context.is_null() {
            return 1;
        }
        // This pointer is to Input's owned Value, alive for the synchronous C++
        // call. The verifier reads metadata only; a panic never crosses the ABI.
        history_result(|| unsafe { crate::db::verify_ui_history_gate(&*context.cast::<Value>()) })
    }
    pub(super) fn recall(pid: u32, base: usize, input: Input) -> Result<()> {
        let context: Vec<_> = input
            .context
            .iter()
            .map(|(text, outgoing, target)| Text {
                text: text.as_ptr(),
                length: text.len(),
                outgoing: u32::from(*outgoing),
                target: u32::from(*target),
            })
            .collect();
        let roots: Vec<_> = input
            .account
            .roots
            .iter()
            .map(|x| AccountRoot {
                landmark: x[0],
                root_slot: x[1],
                root: x[2],
                config: x[3],
            })
            .collect();
        let unique_contexts: Vec<_> = input
            .unique_contexts
            .iter()
            .map(|(start, length)| UniqueContext {
                start: *start,
                length: *length,
            })
            .collect();
        let request = Request {
            pid,
            base,
            group: input.group.as_ptr(),
            group_length: input.group.len(),
            text: input.text.as_ptr(),
            text_length: input.text.len(),
            create_time: input.created,
            context: context.as_ptr(),
            context_length: context.len(),
            unique_contexts: unique_contexts.as_ptr(),
            unique_context_count: unique_contexts.len(),
            history_context: (&input.history_gate as *const Value).cast(),
            validate_history: Some(validate_history),
            account: AccountGate {
                pid: input.account.pid,
                module_base: input.account.module_base,
                wxid: input.account.wxid.as_ptr(),
                wxid_length: input.account.wxid.len(),
                roots: roots.as_ptr(),
                root_count: roots.len(),
            },
        };
        let mut detail = 0;
        // All pointers refer to owned buffers alive through the synchronous call.
        let result = unsafe { wl_chat_ui_recall(&request, &mut detail) };
        let reason = match detail {
            1 => "原生撤回定位信息无效",
            2 => "微信版本或无障碍开关校验不匹配",
            3 => "微信无障碍控件暂不可用",
            4 => "未能精确打开目标群，请先在微信会话列表中显示该群",
            5 => "用户正在操作微信或原生界面发生变化，已停止自动操作",
            6 => "可见消息与数据库文本序列无法唯一对应，未撤回",
            7 => "原生菜单已变化",
            8 => "未找到唯一可用的原生撤回菜单",
            9 => "原生撤回调用结果待数据库确认",
            10 => "微信无障碍状态恢复失败",
            11 => "另一项原生撤回正在进行",
            12 => "微信当前登录账号已变化，未执行撤回",
            13 => "微信聊天历史已变化，请重新操作撤回",
            _ => "原生撤回结果待数据库确认",
        };
        match result {
            0 => Ok(()),
            1 => Err(not_submitted(reason)),
            _ => Err(anyhow!(reason)),
        }
    }
    #[cfg(test)]
    #[test]
    fn cpp_matching_and_guards_are_offline() {
        assert_eq!(unsafe { wl_chat_ui_selftest() }, 0);
    }
    #[cfg(test)]
    #[test]
    fn history_callback_rejects_failure_or_panic_without_crossing_ffi() {
        assert_eq!(history_result(|| Ok(())), 0);
        assert_eq!(history_result(|| Err(anyhow!("fixture"))), 1);
        assert_eq!(history_result(|| panic!("fixture panic")), 1);
        assert_eq!(unsafe { validate_history(std::ptr::null()) }, 1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn info() -> Value {
        json!({"group_name":"Fixture group","ui_text":"target","create_time":100,"server_id":"42","local_id":2,
            "ui_history_gate":{"fixture":true},
            "ui_account_gate":{"pid":1,"module_base":65536,"wxid":"fixture","roots":[{"landmark":65552,"root_slot":65568,"root":65584,"config":65600}]},
            "ui_context":[{"text":"before","outgoing":false,"target":false},{"text":"target","outgoing":true,"target":true},{"text":"after","outgoing":false,"target":false}],
            "ui_unique_contexts":[{"start":0,"length":3}]})
    }
    #[test]
    fn requires_fresh_unique_owned_context_before_process_access() {
        assert!(input(&info(), 279).is_ok());
        assert!(input(&info(), 280).is_err());
        let mut value = info();
        value["ui_context"][1]["outgoing"] = json!(false);
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["ui_context"][0]["target"] = json!(true);
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["ui_text"] = json!("other");
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["forbidden_to_retry"] = json!(true);
        assert!(input(&value, 100).is_err());
    }
    #[test]
    fn rejects_missing_group_and_invalid_locator_without_native_calls() {
        let mut value = info();
        value["group_name"] = Value::Null;
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["server_id"] = json!("not-an-id");
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["local_id"] = json!(0);
        assert!(input(&value, 100).is_err());
        assert!(
            recall(&json!({}))
                .unwrap_err()
                .downcast_ref::<crate::recall::DefinitelyNotSubmitted>()
                .is_some()
        );
    }
    #[test]
    fn requires_valid_global_uniqueness_proofs_covering_the_target() {
        for proofs in [
            Value::Null,
            json!([]),
            json!([{"start":0,"length":0}]),
            json!([{"start":0,"length":4}]),
            json!([{"start":-1,"length":3}]),
            json!([{"start":1,"length":3}]),
            json!([{"start":0,"length":3},{"start":0,"length":3}]),
        ] {
            let mut value = info();
            value["ui_unique_contexts"] = proofs;
            assert!(input(&value, 100).is_err());
        }
        let mut value = info();
        value["ui_context"].as_array_mut().unwrap().push(json!({
            "text":"tail", "outgoing":false, "target":false
        }));
        value["ui_unique_contexts"] = json!([{"start":0,"length":3},{"start":1,"length":3}]);
        assert_eq!(
            input(&value, 100).unwrap().unique_contexts,
            vec![(0, 3), (1, 3)]
        );
        value["ui_context"][2]["break_before"] = json!(true);
        assert!(input(&value, 100).is_err());
        let mut value = info();
        value["ui_context"][0]["break_before"] = json!(true);
        assert!(input(&value, 100).is_ok()); // A break before the proof itself is harmless.
        value["ui_history_gate"] = Value::Null;
        assert!(input(&value, 100).is_err());
    }
    #[test]
    fn accepts_first_text_with_a_global_unique_single_message_proof() {
        let mut value = info();
        let target = value["ui_context"][1].clone();
        value["ui_context"] = json!([target]);
        value["ui_unique_contexts"] = json!([{"start":0,"length":1}]);
        assert_eq!(input(&value, 100).unwrap().unique_contexts, vec![(0, 1)]);
        value["ui_unique_contexts"] = json!([{"start":0,"length":2}]);
        assert!(input(&value, 100).is_err());
    }
}

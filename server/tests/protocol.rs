//! Real HTTP/WS and subprocess regressions. No WeChat/native module or external network is used.
//! `harness=false` lets this same executable serve as the private NDJSON fixture adapter.
mod support;

use anyhow::Result;
use std::{
    future::Future,
    pin::Pin,
    time::{Duration, Instant},
};

type Case = (
    &'static str,
    fn() -> Pin<Box<dyn Future<Output = Result<()>> + Send>>,
);

#[tokio::main]
async fn main() -> std::process::ExitCode {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    if args.first().is_some_and(|s| s == "--adapter") {
        let result = (|| {
            support::adapter::run(
                args.get(1)
                    .ok_or_else(|| anyhow::anyhow!("missing adapter mode"))?,
                std::path::Path::new(
                    args.get(2)
                        .ok_or_else(|| anyhow::anyhow!("missing adapter directory"))?,
                ),
            )
        })();
        if result.is_err() {
            eprintln!("offline fixture adapter failed");
            return std::process::ExitCode::FAILURE;
        }
        return std::process::ExitCode::SUCCESS;
    }
    macro_rules! case {
        ($module:ident::$name:ident) => {
            (
                concat!(stringify!($module), "::", stringify!($name)),
                || Box::pin(support::cases::$module::$name()),
            )
        };
    }
    let cases:Vec<Case>=vec![
        case!(demo::access_key_is_required_for_messages_and_files),
        case!(demo::two_clients_receive_identical_confirmed_message),
        case!(demo::retry_is_idempotent_but_changed_content_is_rejected),
        case!(demo::mention_must_match_actual_member_and_visible_text),
        case!(demo::file_bytes_names_and_date_folders),
        case!(demo::reconnect_replays_missed_events_in_order),
        case!(reliability::local_shutdown_closes_websockets_and_reaps_adapter),
        case!(reliability::different_configs_cannot_share_data_and_lock_releases_after_exit),
        case!(reliability::invalid_tls_and_occupied_port_do_not_open_database_or_start_adapter),
        case!(reliability::new_clients_skip_old_events_and_future_cursor_resets),
        case!(reliability::tls_shutdown_closes_websocket_and_adapter),
        case!(reliability::parallel_same_id_generates_one_queue_and_confirmation_event),
        case!(reliability::search_matches_visible_text_sender_and_filename_only),
        case!(reliability::concurrent_messages_reach_both_clients_in_durable_order),
        case!(authorization::default_empty_local_management_validation_and_restart),
        case!(authorization::history_members_media_old_cursors_and_live_events_cannot_bypass_selection),
        case!(authorization::deselection_cancels_queued_send_and_confirms_submitted_send_without_retry),
        case!(authorization::recoverable_credentials_defaults_and_restart_preserve_global_policy),
        case!(authorization::credential_rotation_preserves_policy_invalidates_old_sessions_and_revocation_erases_secret),
        case!(authorization::legacy_hash_only_key_learns_original_token_after_successful_authentication_and_restart),
        case!(authorization::client_key_never_gains_local_administrator_and_spoofed_headers_do_not_help),
        case!(authorization::group_send_download_quote_history_and_replay_boundaries),
        case!(authorization::permission_update_closes_only_changed_session_and_reconnect_filters_old_and_live_events),
        case!(authorization::revocation_cancels_only_owned_queued_work_and_never_retries_submitted_send),
        case!(actions::locate_requires_current_group_selection_and_never_returns_recalled_content),
        case!(actions::quote_canonical_snapshot_cross_group_forgery_and_withdrawal),
        case!(actions::recall_requires_token_selection_owner_confirmation_and_time_window),
        case!(actions::unsupported_background_recall_is_advertised_in_http_ws_and_never_queued),
        case!(actions::native_quote_and_reply_have_independent_utf8_capacity_before_enqueue),
        case!(actions::pending_recall_survives_restart_as_check_and_never_reissues_action),
        case!(actions::failed_or_uncertain_confirmation_preserves_body_and_does_not_retry),
        case!(actions::only_proven_unsubmitted_recall_can_be_explicitly_retried_with_fresh_authorization),
        case!(history::pages_beyond_initial_40_are_ordered_deduplicated_and_end_only_after_native_eof),
        case!(history::timeout_is_pending_and_same_cursor_requests_share_one_native_task),
        case!(history::group_and_before_validation_read_only_client_and_mid_request_revocation),
        case!(history::native_error_is_retryable_failure_and_does_not_persist_false_eof),
        case!(history::history_events_remain_marked_in_live_broadcast_and_durable_replay),
        case!(records::records_reach_native_eof_without_duplicates_lost_tail_or_insert_order_sorting),
        case!(records::filtered_empty_pages_keep_advancing_until_an_older_native_match),
        case!(records::announcements_are_readonly_authorized_and_distinguish_unset_from_unavailable),
        case!(records::announcement_timeout_deduplicates_waiters_and_ignores_the_old_generation),
        case!(reliability::unconfirmed_send_restart_is_uncertain_and_not_retried),
        case!(reliability::adapter_reconnect_recovers_startup_and_never_retries_unconfirmed_send),
        case!(reliability::adapter_reconnect_shutdown_interrupts_backoff_without_another_child),
    ];
    let mut options = args.iter();
    let mut filter = None;
    while let Some(option) = options.next() {
        if ["--test-threads", "--color", "--format"].contains(&option.as_str()) {
            options.next();
        } else if !option.starts_with('-') {
            filter = Some(option);
            break;
        }
    }
    let list = args.iter().any(|a| a == "--list");
    let cases = cases
        .into_iter()
        .filter(|(name, _)| filter.is_none_or(|s| name.contains(s)))
        .collect::<Vec<_>>();
    if list {
        for (name, _) in &cases {
            println!("{name}: test");
        }
        return std::process::ExitCode::SUCCESS;
    }
    println!("running {} tests", cases.len());
    let start = Instant::now();
    let mut failed = 0;
    for (name, case) in &cases {
        let mut task = tokio::spawn(case());
        let result = tokio::time::timeout(Duration::from_secs(45), &mut task).await;
        if result.is_err() {
            task.abort();
            let _ = task.await;
        }
        match result {
            Ok(Ok(Ok(()))) => println!("test {name} ... ok"),
            other => {
                failed += 1;
                eprintln!("test {name} ... FAILED: {other:?}");
            }
        }
    }
    println!(
        "test result: {}. {} passed; {failed} failed; finished in {:.2}s",
        if failed == 0 { "ok" } else { "FAILED" },
        cases.len() - failed,
        start.elapsed().as_secs_f64()
    );
    if failed == 0 {
        std::process::ExitCode::SUCCESS
    } else {
        std::process::ExitCode::FAILURE
    }
}

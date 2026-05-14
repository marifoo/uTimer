#ifndef TEST_TIMETRACKER_H
#define TEST_TIMETRACKER_H

#include <QObject>
#include "testcommon.h"

class TimeTrackerTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;
    
    void resetDatabaseFile() const
    {
        if (QFile::exists(db_path_)) {
            QFile::remove(db_path_);
        }
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    
    // Existing TimeTracker tests
    void test_timetracker_start_pause_resume_stop_and_checkpoints();
    void test_timetracker_backpause_resets_checkpoint_and_splits();
    void test_timetracker_lock_events_checkpoint_and_resume();
    void test_timetracker_ongoing_duration();
    void test_timetracker_set_duration_type();
    void test_timetracker_checkpoints_paused();
    void test_timetracker_retry_append_failure_then_success_preserves_segments();
    void test_timetracker_retry_failure_keeps_unsaved_state_and_durations();

    // SessionState transition tests
    void test_session_state_begin_new_segment();
    void test_session_state_clear_segment();
    void test_session_state_mark_and_clear_unsaved();
    void test_session_state_reset_for_new_session();
    void test_session_state_adopt_ongoing_segment();

    // State transition tests for public TimeTracker methods
    void test_session_state_start_to_pause_transition();
    void test_session_state_pause_to_activity_transition();
    void test_session_state_stop_clears_segment();

    // Boot time + hasEntriesForDate tri-state tests (T18)
    void test_boot_time_not_added_when_history_disabled();
    void test_boot_time_added_once_on_empty_db();
    void test_boot_time_not_added_when_db_has_entries_for_today();

    // Pause persistence crash safety (T19)
    void test_pause_row_persisted_immediately_on_resume();

    // Midnight forced-stop and cross-midnight discard tests
    void test_addduration_appends_single_same_day_segment();
    void test_addduration_discards_cross_midnight_segment();
    void test_addduration_discards_zero_and_negative_duration();

    void test_isOngoingSegmentCrossMidnight_returns_false_when_stopped();
    void test_isOngoingSegmentCrossMidnight_returns_false_for_same_day();
    void test_isOngoingSegmentCrossMidnight_returns_true_when_segment_started_yesterday();

    void test_useTimerViaButton_force_stops_on_cross_midnight_ongoing();
    void test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight();

    void test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard();

    void test_saveCheckpointInternal_does_not_write_cross_midnight_row();

    void test_stop_clears_was_active_before_autopause();

    void test_startTimer_drops_cross_midnight_boot_time_entry();

    void test_getOngoingDuration_returns_nullopt_when_cross_midnight();

    // Phase 1 test gate (T1)
    // Test D: regression guard — exercises all remaining public methods.
    void test_D_timetracker_public_surface_regression();
    // Test E: boot-time gate via FakeDatabaseManager for each tri-state result.
    void test_E_boot_time_gate_entries_yes_skips_boot_time();
    void test_E_boot_time_gate_entries_no_adds_boot_time();
    void test_E_boot_time_gate_entries_unknown_skips_boot_time();

    // Phase 4 test gate (T4)
    // Test X: stop persists state via commitSession only.
    void test_X_stop_persists_via_commitSession_only();
};

#endif // TEST_TIMETRACKER_H

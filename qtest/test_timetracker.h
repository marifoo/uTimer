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
    void test_timetracker_midnight_split_and_checkpoint_reset();
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

    // computeMidnightSplit pure function tests
    void test_compute_midnight_split_no_crossing();
    void test_compute_midnight_split_crossing();
    void test_compute_midnight_split_zero_duration();

    // Boot time + hasEntriesForDate tri-state tests (T18)
    void test_boot_time_not_added_when_history_disabled();
    void test_boot_time_added_once_on_empty_db();
    void test_boot_time_not_added_when_db_has_entries_for_today();
};

#endif // TEST_TIMETRACKER_H

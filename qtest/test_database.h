#ifndef TEST_DATABASE_H
#define TEST_DATABASE_H

#include <QObject>
#include "testcommon.h"

class DatabaseTest : public QObject
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
    
    // Core database tests
    void test_database_backups_and_retention_and_disable();
    void test_databasemanager_write_failure();
    void test_database_transaction_rollback_on_insert_failure();
    void test_database_transaction_rollback_on_replace_failure();
    void test_database_checkpoint_single_row_per_segment();
    void test_database_checkpoint_missing_segment_reinserts_same_segment_id();
    void test_database_checkpoint_preserves_start_time();
    void test_database_update_by_id_insert_mode();
    void test_database_update_by_id_updates_existing_row_on_start_drift();
    void test_database_update_by_id_different_segments_same_start_both_exist();
    void test_database_update_by_id_empty_deque();
    void test_database_load_negative_duration();
    void test_database_load_start_after_end();
    void test_database_load_invalid_enum_type();
    void test_database_load_duration_mismatch_tolerance();
    void test_database_load_invalid_type_increments_skipped_and_omits_row();
    void test_database_load_200ms_mismatch_increments_repaired_and_uses_computed();
    void test_database_timezone_roundtrip();
    void test_database_millisecond_precision();
    void test_database_schema_validation_missing_start_date();
    void test_database_schema_validation_fresh_database();
    void test_database_schema_migration_adds_is_finalized_and_segment_id_marks_existing_rows();
    void test_database_backup_file_creation();
    void test_database_backup_preserves_data();
    
    // Schema/validation tests
    void test_schemaValidation_missingStartColumns();
    void test_exactMatching_upsertReplacesById();
    void test_explicitStartTimes_constructorComputesDuration();
    void test_explicitStartTimes_startTimePreservedAfterClean();
    void test_explicitStartTimes_mergeUpdatesAllFields();
    void test_splitPreservesStartTime();
    void test_zeroDurationNotAdded();
    void test_negativeDurationHandled();
    void test_checkpointPreservesStartTimeOnUpdateBySegmentId();
    void test_clockDriftResilience_durationStoredFromElapsed();
    void test_loadDurations_skipsNegativeDurationRows();

    // hasEntriesForDate tri-state result tests (T18)
    void test_hasEntriesForDate_returns_unknown_when_history_disabled();
    void test_hasEntriesForDate_returns_no_on_empty_database();
    void test_hasEntriesForDate_returns_yes_when_entries_exist();

    // Retention cleanup once-per-session tests (T22)
    void test_retention_cleanup_runs_once_across_multiple_opens();
    void test_retention_cleanup_retries_after_failure();
};

#endif // TEST_DATABASE_H

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
    void test_database_checkpoint_id_reuse();
    void test_database_checkpoint_deleted_row_creates_new();
    void test_database_checkpoint_preserves_start_time();
    void test_database_upsert_insert_mode();
    void test_database_upsert_replace_mode();
    void test_database_upsert_unique_constraint();
    void test_database_upsert_empty_deque();
    void test_database_load_negative_duration();
    void test_database_load_start_after_end();
    void test_database_load_invalid_enum_type();
    void test_database_load_duration_mismatch_tolerance();
    void test_database_timezone_roundtrip();
    void test_database_millisecond_precision();
    void test_database_schema_validation_missing_start_date();
    void test_database_schema_validation_fresh_database();
    void test_database_backup_file_creation();
    void test_database_backup_preserves_data();
    
    // Schema/validation tests
    void test_schemaValidation_missingStartColumns();
    void test_exactMatching_upsertReplacesByStartTime();
    void test_explicitStartTimes_constructorComputesDuration();
    void test_explicitStartTimes_startTimePreservedAfterClean();
    void test_explicitStartTimes_mergeUpdatesAllFields();
    void test_splitPreservesStartTime();
    void test_zeroDurationNotAdded();
    void test_negativeDurationHandled();
    void test_checkpointPreservesStartTimeOnUpdate();
    void test_clockDriftResilience_durationStoredFromElapsed();
    void test_loadDurations_skipsNegativeDurationRows();
};

#endif // TEST_DATABASE_H

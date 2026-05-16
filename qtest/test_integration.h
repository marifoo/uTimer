#ifndef TEST_INTEGRATION_H
#define TEST_INTEGRATION_H

#include <QObject>
#include "testcommon.h"

class IntegrationTest : public QObject
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
    
    void test_integration_checkpoint_recovery_on_restart();
    void test_integration_orphan_reconciliation_is_idempotent();
    void test_integration_orphan_reconciliation_drops_stale_and_too_short();
    void test_integration_orphan_reconciliation_marker_present_is_silent();
    void test_integration_orphan_reconciliation_marker_absent_shows_notification();
    void test_integration_memory_db_consistency();
    void test_integration_retention_cleanup_preserves_current();
    void test_integration_duplicate_prevention();
    void test_integration_empty_database_operations();
    void test_integration_backpause_db_update();

    // Phase 1 test gate (T1)
    // Test F: shutdown sequence end-to-end via db_ directly.
    void test_F_shutdown_sequence_stop_flush_marker();

    // Phase 5 pre-flight regression tests (T5.0a)
    // Run on pre-refactor code as a safety net for Phase 5 changes.
    void test_5_0a_normal_same_day_no_discard();
    void test_5_0a_cross_midnight_ongoing_discarded_completed_preserved();
    void test_5_0a_discard_is_idempotent();
    void test_5_0a_watchdog_helper_returns_false_when_not_crossed();

    // Phase 5 test gate (Tests Y–AB)
    void test_Y_engine_scheduled_stop_emits_MidnightScheduled();
    void test_Z_engine_watchdog_emits_MidnightWatchdog();
    void test_AA_no_duplicate_stop_signal();
    void test_AB_scheduleMidnightStop_is_gone();

    // Step 3 (S2 + T3): atomic overlap-guarded finalisation.
    void test_fakeStore_finalizeIfNoOverlap_mirrors_sqlite_semantics();
    void test_fakeStore_reconcile_reports_finalized_and_overlap_dropped();
    void test_timer_reconcileOrphans_excludes_overlap_dropped_from_recovered_seconds();
};

#endif // TEST_INTEGRATION_H

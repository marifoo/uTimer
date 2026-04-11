#ifndef TEST_HISTORYDIALOG_H
#define TEST_HISTORYDIALOG_H

#include <QObject>
#include "testcommon.h"

class HistoryDialogTest : public QObject
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

    void test_historydialog_createPages_includes_current_db_ongoing();
    void test_historydialog_createPages_dedups_db_row_with_small_time_drift();
    void test_historydialog_checkbox_toggle_updates_pending_and_totals();
    void test_historydialog_saveChanges_updates_timetracker_and_db();
    void test_historydialog_split_action_splits_row();
    void test_historydialog_split_today_mixed_origins_routes_to_correct_bucket();
    void test_historydialog_split_non_today_db_row_survives_save_roundtrip();
    void test_historydialog_shows_load_reconciliation_banner();
    void test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint();
    void test_historydialog_pauses_checkpoint_timer_for_dialog_lifetime();
    void test_historydialog_save_keeps_db_rows_for_history_plus_current_session();
    void test_historydialog_save_then_crash_reopen_retains_current_segment_row();
    void test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait();

    void test_splitdialog_default_types_and_bounds();
    void test_splitdialog_short_duration_disables_slider();
    void test_splitdialog_setters_affect_types();
};

#endif // TEST_HISTORYDIALOG_H

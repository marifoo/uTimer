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
    void test_historydialog_checkbox_toggle_updates_pending_and_totals();
    void test_historydialog_saveChanges_updates_timetracker_and_db();
    void test_historydialog_split_action_splits_row();

    void test_splitdialog_default_types_and_bounds();
    void test_splitdialog_short_duration_disables_slider();
    void test_splitdialog_setters_affect_types();
};

#endif // TEST_HISTORYDIALOG_H

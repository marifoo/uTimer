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
    
    void test_timetracker_start_pause_resume_stop_and_checkpoints();
    void test_timetracker_backpause_resets_checkpoint_and_splits();
    void test_timetracker_midnight_split_and_checkpoint_reset();
    void test_timetracker_lock_events_checkpoint_and_resume();
    void test_timetracker_ongoing_duration();
    void test_timetracker_set_duration_type();
    void test_timetracker_checkpoints_paused();
};

#endif // TEST_TIMETRACKER_H

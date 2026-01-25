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
    void test_integration_memory_db_consistency();
    void test_integration_retention_cleanup_preserves_current();
    void test_integration_duplicate_prevention();
    void test_integration_empty_database_operations();
    void test_integration_backpause_db_update();
};

#endif // TEST_INTEGRATION_H

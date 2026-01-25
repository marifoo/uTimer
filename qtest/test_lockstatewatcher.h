#ifndef TEST_LOCKSTATEWATCHER_H
#define TEST_LOCKSTATEWATCHER_H

#include <QObject>
#include "testcommon.h"

class LockStateWatcherTest : public QObject
{
    Q_OBJECT

private slots:
    void test_lockstatewatcher_debounce_logic();
};

#endif // TEST_LOCKSTATEWATCHER_H

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <QObject>
#include "testcommon.h"

class HelpersTest : public QObject
{
    Q_OBJECT

private slots:
    void test_helpers_conversions();
};

#endif // TEST_HELPERS_H

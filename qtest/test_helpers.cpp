#include "test_helpers.h"
#include <QtTest>

void HelpersTest::test_helpers_conversions()
{
    // convMSecToTimeStr
    QCOMPARE(convMSecToTimeStr(3661000), QString("01:01:01"));
    QCOMPARE(convMSecToTimeStr(0), QString("00:00:00"));

    // convMinAndSecToHourPctString
    // 30 min = 0.5 hours -> "50" (dot is added by caller)
    QCOMPARE(convMinAndSecToHourPctString(30, 0), QString("50"));
    // 15 min = 0.25 hours -> "25"
    QCOMPARE(convMinAndSecToHourPctString(15, 0), QString("25"));
    // 45 min = 0.75 hours -> "75"
    QCOMPARE(convMinAndSecToHourPctString(45, 0), QString("75"));

    // convTimeStrToDurationStr
    QCOMPARE(convTimeStrToDurationStr("1:30:00"), QString("1.50"));
    QCOMPARE(convTimeStrToDurationStr("0:15:00"), QString("0.25"));
}

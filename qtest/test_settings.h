#ifndef TEST_SETTINGS_H
#define TEST_SETTINGS_H

#include <QObject>
#include "testcommon.h"

class SettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_settings_defaults_and_bounds();
    // 9.4: unknown keys survive a load/save round-trip; sync failure is reported
    void test_settings_unknown_key_survives_roundtrip();
    void test_settings_sync_failure_reported();
};

#endif // TEST_SETTINGS_H

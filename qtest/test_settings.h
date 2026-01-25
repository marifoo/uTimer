#ifndef TEST_SETTINGS_H
#define TEST_SETTINGS_H

#include <QObject>
#include "testcommon.h"

class SettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_settings_defaults_and_bounds();
};

#endif // TEST_SETTINGS_H

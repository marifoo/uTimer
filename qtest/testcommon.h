#ifndef TESTCOMMON_H
#define TESTCOMMON_H

#include <QtTest>
#include <deque>
#include <iterator>
#include <cmath>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtDebug>

// Expose private members for testing
#define private public
#define protected public
#include "timetracker.h"
#include "lockstatewatcher.h"
#undef private
#undef protected

#include "databasemanager.h"
#include "helpers.h"
#include "settings.h"

namespace TestCommon {

// Factory function for creating TimeDuration objects from millisecond timestamps
TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs);

// Creates a test settings file with specified history days retention
QString createSettingsFile(const QString& dirPath, int historyDays);

// Sums all durations of a specific type from a deque
qint64 sumDurations(const std::deque<TimeDuration>& d, DurationType type);

} // namespace TestCommon

#endif // TESTCOMMON_H

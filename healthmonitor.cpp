#include "healthmonitor.h"

HealthMonitor::HealthMonitor(const Settings& settings, QObject* parent)
    : QObject(parent), settings_(settings),
      activity_warning_shown_(false), pause_warning_shown_(false)
{
}

void HealthMonitor::check(qint64 /*activeMsec*/, qint64 /*pauseMsec*/)
{
    // stub — body moved in T2.5
}

void HealthMonitor::reset()
{
    // stub — body moved in T2.6
}

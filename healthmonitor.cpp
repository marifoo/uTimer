#include "healthmonitor.h"
#include "helpers.h"

HealthMonitor::HealthMonitor(const Settings& settings, QObject* parent)
    : QObject(parent), settings_(settings),
      activity_warning_shown_(false), pause_warning_shown_(false)
{
}

void HealthMonitor::check(qint64 activeMsec, qint64 pauseMsec)
{
    if (!activity_warning_shown_ && activeMsec > settings_.getWarnTimeActivityMsec()) {
        activity_warning_shown_ = true;
        emit warningTriggered("Total activity time: " + convMSecToTimeStr(activeMsec));
    }

    if (!pause_warning_shown_
        && activeMsec > settings_.getWarnTimeNoPauseMsec()
        && pauseMsec < settings_.getPauseTimeForWarnTimeNoPauseMsec()) {
        pause_warning_shown_ = true;
        emit warningTriggered("Pause time: " + convMSecToTimeStr(pauseMsec)
                              + "\nwith activity time: " + convMSecToTimeStr(activeMsec));
    }
}

void HealthMonitor::reset()
{
    activity_warning_shown_ = !settings_.showTooMuchActivityWarning();
    pause_warning_shown_ = !settings_.showNoPauseWarning();
}

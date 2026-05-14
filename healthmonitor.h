#ifndef HEALTHMONITOR_H
#define HEALTHMONITOR_H

#include <QObject>
#include <QString>
#include "settings.h"

class HealthMonitor : public QObject
{
    Q_OBJECT
public:
    explicit HealthMonitor(const Settings& settings, QObject* parent = nullptr);

    void check(qint64 activeMsec, qint64 pauseMsec);
    void reset();

signals:
    void warningTriggered(const QString& text);

private:
    const Settings& settings_;
    bool activity_warning_shown_;
    bool pause_warning_shown_;
};

#endif // HEALTHMONITOR_H

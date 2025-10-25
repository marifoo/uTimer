#ifndef HELPERS
#define HELPERS

#include <QtGlobal>
#include <QString>
#include <QDateTime>
#include <QPushButton>
#include <QColor>
#include <deque>
#include "types.h"


qint64 convMinToMsec(const int &minutes);

QString convMSecToTimeStr(const qint64 &time);

void toggleButtonColor(QPushButton * const button, const QColor &color);

QString convMinAndSecToHourPctString(const int min, const int sec);

QString convTimeStrToDurationStr(const QString &time_str);

void cleanDurations(std::deque<TimeDuration>* pDurations);

#endif // HELPERS

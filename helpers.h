#ifndef HELPERS
#define HELPERS

#include <QtGlobal>
#include <QString>
#include <QDateTime>
#include <QPushButton>
#include <QColor>
#include <deque>
#include <vector>
#include "types.h"


qint64 convMinToMsec(const int &minutes);

QString convMSecToTimeStr(const qint64 &time);

void toggleButtonColor(QPushButton * const button, const QColor &color);

QString convMinAndSecToHourPctString(const int min, const int sec);

QString convTimeStrToDurationStr(const QString &time_str);

std::vector<QString> cleanDurations(std::deque<TimeDuration>* pDurations);

/// Returns dt.toUTC().toString(Qt::ISODateWithMs).
/// All UTC timestamps stored in the database use this canonical format,
/// which sorts lexicographically in chronological order.
QString toUtcIso(const QDateTime& dt);

#endif // HELPERS

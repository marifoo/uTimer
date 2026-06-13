#ifndef TIMEFORMAT_H
#define TIMEFORMAT_H

#include <QtGlobal>
#include <QString>

/// Converts minutes to milliseconds.
qint64 convMinToMsec(const int& minutes);

/// Formats a millisecond duration as "hh:mm:ss".
QString convMSecToTimeStr(const qint64& time);

/// Formats (minutes, seconds) as a two-digit hundredths-of-an-hour string.
/// Internal helper for convTimeStrToDurationStr; exposed here to keep the
/// helpers translation unit self-contained.
QString convMinAndSecToHourPctString(const int min, const int sec);

/// Converts a "hh:mm:ss" time string to a decimal-hours string (e.g. "1.50").
QString convTimeStrToDurationStr(const QString& time_str);

#endif // TIMEFORMAT_H

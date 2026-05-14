#include "helpers.h"
#include "timeline.h"
#include <algorithm>
#include <cmath>
#include <vector>


qint64 convMinToMsec(const int &minutes)
{
	return (static_cast<qint64>(minutes) * 60000);
}

QString convMSecToTimeStr(const qint64 &time)
{
	return (QDateTime::fromTime_t(static_cast<unsigned int>(time/1000)).toUTC().toString("hh:mm:ss"));
}

void toggleButtonColor(QPushButton * const button, const QColor &color)
{
	const QString stylesheet_string = QString("QPushButton {background-color: %1;}").arg(color.name());
	if (button->styleSheet() != stylesheet_string)
		button->setStyleSheet(stylesheet_string);
	else
		button->setStyleSheet("");
}

QString convMinAndSecToHourPctString(const int min, const int sec)
{
	return (QString::number((min*60 + sec)/36).rightJustified(2, '0'));
}

QString convTimeStrToDurationStr(const QString &time_str)
{
	const QStringList split = time_str.split(":");
	QString hours = QString::number(split[0].toInt());
	QString hour_frac = convMinAndSecToHourPctString(split[1].toInt(), split[2].toInt());
	return (hours + "." + hour_frac);
}

/**
 * Optimization: Cleans up the duration list before database insertion.
 *
 * Thin wrapper around Timeline::normalized(). The canonical algorithm lives
 * in timeline.cpp; this function collects orphaned segment_ids (those that
 * were merged away) and returns them so callers can remove them from the DB.
 */
std::vector<QString> cleanDurations(std::deque<TimeDuration>* pDurations)
{
	// Collect before-IDs
	std::vector<QString> before;
	for (const auto& d : *pDurations)
		before.push_back(d.segment_id);

	Timeline t(*pDurations, std::nullopt);
	Timeline normed = t.normalized();
	*pDurations = normed.completed();

	// Return IDs that disappeared
	std::vector<QString> removed;
	for (const auto& id : before) {
		bool found = false;
		for (const auto& d : *pDurations)
			if (d.segment_id == id) { found = true; break; }
		if (!found)
			removed.push_back(id);
	}
	return removed;
}

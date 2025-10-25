#include "helpers.h"
#include <algorithm>
#include <cmath>


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

void cleanDurations(std::deque<TimeDuration>* pDurations)
{
	// Clean up duration entries by removing near-duplicates and merging adjacent entries of the same type
	// This prevents database bloat from frequent timer operations
	auto& durations = *pDurations;
	if (durations.size() < 2) {
		return;
	}

	// Ensure chronological order by start time, then end time. This makes merges deterministic.
	std::sort(durations.begin(), durations.end(), [](const TimeDuration& a, const TimeDuration& b) {
		const qint64 a_start = a.endTime.toMSecsSinceEpoch() - a.duration;
		const qint64 b_start = b.endTime.toMSecsSinceEpoch() - b.duration;
		if (a_start != b_start) return a_start < b_start;
		const qint64 a_end = a.endTime.toMSecsSinceEpoch();
		const qint64 b_end = b.endTime.toMSecsSinceEpoch();
		if (a_end != b_end) return a_end < b_end;
		// Prefer longer first when start and end equal, so near-duplicate removal keeps longer
		return a.duration < b.duration;
	});

	for (auto it = durations.begin() + 1; it != durations.end(); ) {
		auto prevIt = std::prev(it);

		// Merge consecutive durations of the same type that are close in time
		if (prevIt->type == it->type) {
			const qint64 prev_start = prevIt->endTime.toMSecsSinceEpoch() - prevIt->duration;
			const qint64 it_start = it->endTime.toMSecsSinceEpoch() - it->duration;
			const qint64 prev_end = prevIt->endTime.toMSecsSinceEpoch();
			const qint64 it_end = it->endTime.toMSecsSinceEpoch();

			const qint64 diff_end = prev_end - it_end;
			const qint64 diff_dur = prevIt->duration - it->duration;
			const qint64 gap = it_start - prev_end;

			// Remove near-duplicate entries (within 50ms difference)
			if (std::abs(diff_end) < 50 && std::abs(diff_dur) < 50) {
				it = durations.erase(it);
				continue;
			}

			// Current entry starts before previous (shorter) entry -> delete
			if (it_start < prev_start && prev_end <= it_end) {
				prevIt->endTime = it->endTime;
				prevIt->duration = it->duration;
				it = durations.erase(it);
				continue;
			}

			// Current entry starts before previous (longer) entry -> join
			if (it_start < prev_start && it_end < prev_end && it_start < prev_end) {
				prevIt->duration = prev_end - it_start;
				it = durations.erase(it);
				continue;
			}

			// Current entry start overlaps into previous entry -> join (includes touching)
			if (prev_start <= it_start && it_start <= prev_end && prev_end <= it_end) {
				prevIt->endTime = it->endTime;
				prevIt->duration = it_end - prev_start;
				it = durations.erase(it);
				continue;
			}

			// Current entry start is subset of previous entry -> delete
			if (prev_start <= it_start && it_start <= prev_end && it_end <= prev_end) {
				it = durations.erase(it);
				continue;
			}

			// Merge adjacent (but disjoint) entries of same type with small gaps (less than 500ms)
			if (gap >= 0 && gap < 500) {
				prevIt->duration += it->duration + gap;
				prevIt->endTime = it->endTime;
				it = durations.erase(it);
				continue;
			}

			// Slightly overlapping entries shall be merged as well (less than 100ms overlap)
			if (gap < 0 && std::abs(gap) < 100) {
				prevIt->duration = it_end - prev_start;
				prevIt->endTime = it->endTime;
				it = durations.erase(it);
				continue;
			}
		}
		++it;
	}
}

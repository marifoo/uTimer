#include "helpers.h"



qint64 convMinToMsec(const int &minutes)
{
	return (static_cast<qint64>(minutes) * 60000);
}

QString convMSecToTimeStr(const qint64 &time)
{
	return (QDateTime::fromTime_t(time/1000).toUTC().toString("hh:mm:ss"));
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

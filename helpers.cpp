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

QString convTimeStrToDurationStr(const QString &time_str)
{
	const QStringList split = time_str.split(":");

	QString hours = split[0];
	if (hours.startsWith("0"))
		hours.remove(0,1);

	QString hour_frac = QString::number(static_cast<int>(float(100.0) * (split[1].toFloat()*60.0f + split[2].toFloat())/3600.0f));
	if (hour_frac.length() == 1)
		hour_frac = "0"+hour_frac;

	return (hours + "." + hour_frac);
}

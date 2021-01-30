#ifndef CONTENTWIDGET_H
#define CONTENTWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QPushButton>
#include "settings.h"
#include "types.h"

class ContentWidget : public QWidget
{
	Q_OBJECT
private:
	QVBoxLayout *rows_;
	QHBoxLayout *activity_row_;
	QLabel *activity_text_;
	QLabel *activity_time_;
	QHBoxLayout *pause_row_;
	QLabel *pause_text_;
	QLabel *pause_time_;
	QHBoxLayout *button_row_;
	QPushButton *startpause_button_;
	QPushButton *stop_button_;
	QHBoxLayout *optionbutton_row_;
	QPushButton *mintotray_button_;
	QPushButton * pintotop_button_;
	QPushButton *autopause_button_;
	Qt::GlobalColor button_hold_color_;
	QString autopause_tooltip_;
	QString activity_time_tooltip_base_;

	Settings & settings_;

	void setupGUI();
	void setupTimeRows();
	void setupButtonRows();
	void applyStartupSettingsToGui();
	void doButtonColorToggle(QPushButton * button, const QColor &color);
	QString convertTimeStrToDurationStr(const QString &activity) const;
	void setActivityTimeTooltip(const QString &hours = "0.00");
	void setPauseTimeTooltip();
	void resetPauseTimeTooltip();

public:
	explicit ContentWidget(Settings & settings, QWidget *parent = nullptr);
	void setAllTimes(const QString &activity, const QString &pause);
	QString getTooltip();
	bool isGUIinActivity();

signals:
	void minToTray();
	void toggleAlwaysOnTop();
	void pressedButton(Button button);

public slots:
	void pressedStartPauseButton();
	void pressedStopButton();
	void pressedMinToTrayButton();
	void pressedPinToTopButton();
	void pressedAutoPauseButton();
	void setGUItoActivity();
	void setGUItoStop();
	void setGUItoPause();
};

#endif // CONTENTWIDGET_H

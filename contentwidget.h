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
#include "timetracker.h"


class ContentWidget : public QWidget
{
	Q_OBJECT

private:
	Settings &settings_;
	TimeTracker &timetracker_;

	QVBoxLayout *rows_;
	QHBoxLayout *activity_row_;
	QLabel *activity_text_;
	QLabel *activity_time_;
	QHBoxLayout *pause_row_;
	QLabel *pause_text_;
	QLabel *pause_time_;
	QHBoxLayout *timerbutton_row_;
	QPushButton *startpause_button_;
	QPushButton *stop_button_;
	QHBoxLayout *historybutton_row_;
	QPushButton *show_history_button_;
	QPushButton *autopause_button_;
	QHBoxLayout* optionbutton_row_;
	QPushButton* mintotray_button_;
	QPushButton* pintotop_button_;
	const QColor button_hold_color_;
	QString autopause_tooltip1_;
	QString autopause_tooltip2_;
	QString activity_time_tooltip_base_;

	void setupGUI();
	void setupTimeRows();
	void setupButtonRows();
	void applyStartupSettingsToGui();
	void setActivityTimeTooltip(const QString &hours = "0.00");
	void setPauseTimeTooltip();
	void resetPauseTimeTooltip();
	void manageTooltipsForActivity();
        
public:
	explicit ContentWidget(Settings &settings, TimeTracker& timetracker, QWidget *parent = nullptr);
	void updateTimes();
	QString getTooltip();
	bool isGUIinActivity();
	bool isGUIinPause();

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
	void pressedShowHistoryButton();
	void setGUItoActivity();
	void setGUItoStop();
	void setGUItoPause();
};

#endif // CONTENTWIDGET_H

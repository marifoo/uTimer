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
	std::unique_ptr<QVBoxLayout> rows_;
	std::unique_ptr<QHBoxLayout> activity_row_;
	std::unique_ptr<QLabel> activity_text_;
	std::unique_ptr<QLabel> activity_time_;
	std::unique_ptr<QHBoxLayout> pause_row_;
	std::unique_ptr<QLabel> pause_text_;
	std::unique_ptr<QLabel> pause_time_;
	std::unique_ptr<QHBoxLayout> button_row_;
	std::unique_ptr<QPushButton> startpause_button_;
	std::unique_ptr<QPushButton> stop_button_;
	std::unique_ptr<QHBoxLayout> optionbutton_row_;
	std::unique_ptr<QPushButton> mintotray_button_;
	std::unique_ptr<QPushButton> pintotop_button_;
	std::unique_ptr<QPushButton> autopause_button_;
	Qt::GlobalColor button_hold_color_;
	QString autopause_tooltip_;
	QString activity_time_tooltip_base_;

	Settings & settings_;

	void setupGUI();
	void doButtonColorToggle(std::unique_ptr<QPushButton> & button, QColor color);
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

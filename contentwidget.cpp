#include "contentwidget.h"
#include <QDateTime>
#include <QtDebug>
#include <QFont>
#include <QColor>
#include <QStringList>
#include <QApplication>
#include "helpers.h"

ContentWidget::ContentWidget(Settings & settings, QWidget *parent) : QWidget(parent), settings_(settings), button_hold_color_(QColor(180,216,228,255))
{
	setupGUI();

	QObject::connect(startpause_button_, SIGNAL(clicked()), this, SLOT(pressedStartPauseButton()));
	QObject::connect(stop_button_, SIGNAL(clicked()), this, SLOT(pressedStopButton()));
	QObject::connect(mintotray_button_, SIGNAL(clicked()), this, SLOT(pressedMinToTrayButton()));
	QObject::connect(pintotop_button_, SIGNAL(clicked()), this, SLOT(pressedPinToTopButton()));
	QObject::connect(autopause_button_, SIGNAL(clicked()), this, SLOT(pressedAutoPauseButton()));
}

void ContentWidget::setupGUI()
{
	rows_ = new QVBoxLayout(this);

	setupTimeRows();
	setupButtonRows();

	rows_->addLayout(activity_row_);
	rows_->addLayout(pause_row_);
	rows_->addLayout(button_row_);
	rows_->addLayout(optionbutton_row_);

	applyStartupSettingsToGui();
}

void ContentWidget::setupTimeRows()
{
	QFont label_font = QApplication::font();
	label_font.setPointSize(9);

	// Activity Time:  00:00:00
	activity_row_ = new QHBoxLayout();
	activity_text_ = new QLabel("Activity Time:");
	activity_text_->setFont(label_font);
	activity_time_ = new QLabel("00:00:00");
	activity_time_->setFont(label_font);
	activity_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	activity_row_->addWidget(activity_text_);
	activity_row_->addWidget(activity_time_);

	// Pause Time:  00:00:00
	pause_row_ = new QHBoxLayout();
	pause_text_ = new QLabel("Pause Time:");
	pause_text_->setFont(label_font);
	pause_time_ = new QLabel("00:00:00");
	pause_time_->setFont(label_font);
	pause_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	pause_row_->addWidget(pause_text_);
	pause_row_->addWidget(pause_time_);
}

void ContentWidget::setupButtonRows()
{
	QFont button_font = QApplication::font();
	button_font.setPointSize(8);

	// [START] [STOP]
	button_row_ = new QHBoxLayout();
	startpause_button_ = new QPushButton("START");
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	stop_button_ = new QPushButton("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	button_row_->addWidget(startpause_button_);
	button_row_->addWidget(stop_button_);

	// [Min to Tray] [Stay on Top] [Auto-Pause]
	optionbutton_row_ = new QHBoxLayout();
	mintotray_button_ = new QPushButton("Min to Tray");
	mintotray_button_->setFont(button_font);
	mintotray_button_->setToolTip("Minimize to Tray Icon now");
	pintotop_button_ = new QPushButton("Stay on Top");
	pintotop_button_->setFont(button_font);
	pintotop_button_->setToolTip("Keep this Window in Foreground");
	autopause_button_ = new QPushButton("Auto-Pause");
	autopause_button_->setFont(button_font);
	autopause_tooltip_ = "min after locking the PC, pause the Timer and count this whole time retroactively as a Pause too";
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip_);
	optionbutton_row_->addWidget(mintotray_button_);
	optionbutton_row_->addWidget(pintotop_button_);
	optionbutton_row_->addWidget(autopause_button_);
}

void ContentWidget::applyStartupSettingsToGui()
{
	if (settings_.isAutopauseEnabled())
		toggleButtonColor(autopause_button_, button_hold_color_);

	if (settings_.isPinnedStartEnabled())
		toggleButtonColor(pintotop_button_, button_hold_color_);
}

void ContentWidget::pressedStartPauseButton()
{
	const bool from_activity = (startpause_button_->text() == "PAUSE");
	const bool from_pause_or_stopped = ((startpause_button_->text() == "START") || (startpause_button_->text() == "CONTINUE"));

	if (from_activity) {
		setGUItoPause();
		emit pressedButton(Button::Pause);
	}
	else if (from_pause_or_stopped) {
		setGUItoActivity();
		emit pressedButton(Button::Start);
	}
}

void ContentWidget::pressedStopButton()
{
	setGUItoStop();
	emit pressedButton(Button::Stop);
}

void ContentWidget::pressedMinToTrayButton()
{
	emit minToTray();
}

void ContentWidget::pressedPinToTopButton()
{
	toggleButtonColor(pintotop_button_, button_hold_color_);
	settings_.setPinToTopState(!settings_.isPinnedStartEnabled());
	emit toggleAlwaysOnTop();
}

void ContentWidget::pressedAutoPauseButton()
{
	toggleButtonColor(autopause_button_, button_hold_color_);
	settings_.setAutopauseState(!settings_.isAutopauseEnabled());
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip_);
}

void ContentWidget::setActivityTimeTooltip(const QString &hours /* ="0.00" */)
{
	const QString tooltip_label = "That's " + hours + activity_time_tooltip_base_;
	activity_time_->setToolTip(tooltip_label);
}

void ContentWidget::setPauseTimeTooltip()
{
	const QString tooltip_label = "Last Pause ended at " + QTime::currentTime().toString("hh:mm") + " o'clock";
	pause_time_->setToolTip(tooltip_label);
}

void ContentWidget::resetPauseTimeTooltip()
{
	pause_time_->setToolTip("");
}

void ContentWidget::manageTooltipsForActivity()
{
	const bool from_stopped = (startpause_button_->text() == "START");
	const bool from_pause = (startpause_button_->text() == "CONTINUE");

	if (from_stopped) {
		activity_time_tooltip_base_ = "h overall since " + QTime::currentTime().toString("hh:mm") + " o'clock";
		setActivityTimeTooltip();
		resetPauseTimeTooltip();
	}
	else if (from_pause) {
		setPauseTimeTooltip();
	}
}

void ContentWidget::setGUItoActivity()
{
	manageTooltipsForActivity();

	startpause_button_->setText("PAUSE");
	activity_time_->setStyleSheet("QLabel {color : green; }");
	pause_time_->setStyleSheet("QLabel { color : black; }");
}

void ContentWidget::setGUItoStop()
{
	startpause_button_->setText("START");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : black; }");
}

void ContentWidget::setGUItoPause()
{
	startpause_button_->setText("CONTINUE");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : green; }");
}

void ContentWidget::setAllTimes(const qint64 &t_active, const qint64 &t_pause)
{
	pause_time_->setText(convMSecToTimeStr(t_pause));
	activity_time_->setText(convMSecToTimeStr(t_active));
	setActivityTimeTooltip(convTimeStrToDurationStr(convMSecToTimeStr(t_active)));
}

QString ContentWidget::getTooltip()
{
	if (startpause_button_->text() == "CONTINUE")
		return QString("µTimer:  In Pause (Overall " + pause_time_->text() + ")");
	else if (startpause_button_->text() == "PAUSE")
		return QString("µTimer:  In Activity (Overall " + convTimeStrToDurationStr(activity_time_->text()) + "h / " + activity_time_->text() + ")");
	else
		return QString("µTimer:  Timing inactive");
}

bool ContentWidget::isGUIinActivity()
{
	return (startpause_button_->text() == "PAUSE");
}

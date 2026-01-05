/**
 * ContentWidget - The main GUI presentation layer.
 *
 * Design Pattern: Passive View / Dumb Widget
 * - This widget owns the UI elements (Buttons, Labels) but holds very little logic.
 * - State is primarily managed by TimeTracker and reflected here via polling.
 * - updateTimes() is called by the main loop (MainWin::update) to refresh the display.
 *
 * Interaction:
 * - Button presses emit signals (pressedButton) to MainWin, which forwards them to TimeTracker.
 * - TimeTracker state changes are NOT automatically pushed here; the main loop polls
 *   TimeTracker and calls updateTimes().
 * - setGUItoActivity/Pause/Stop helper methods are used to force the visual state
 *   to match the logical state (e.g., when resuming from tray or changing modes).
 */

#include "contentwidget.h"
#include <QDateTime>
#include <QtDebug>
#include <QFont>
#include <QColor>
#include <QStringList>
#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include "helpers.h"
#include "historydialog.h"

ContentWidget::ContentWidget(Settings & settings, TimeTracker &timetracker, QWidget *parent)
	: QWidget(parent), settings_(settings), timetracker_(timetracker), button_hold_color_(QColor(180,216,228,255))
{
	setupGUI();

	QObject::connect(startpause_button_, SIGNAL(clicked()), this, SLOT(pressedStartPauseButton()));
	QObject::connect(stop_button_, SIGNAL(clicked()), this, SLOT(pressedStopButton()));
	QObject::connect(mintotray_button_, SIGNAL(clicked()), this, SLOT(pressedMinToTrayButton()));
	QObject::connect(pintotop_button_, SIGNAL(clicked()), this, SLOT(pressedPinToTopButton()));
	QObject::connect(autopause_button_, SIGNAL(clicked()), this, SLOT(pressedAutoPauseButton()));
	QObject::connect(show_history_button_, SIGNAL(clicked()), this, SLOT(pressedShowHistoryButton()));
}

void ContentWidget::setupGUI()
{
	rows_ = new QVBoxLayout(this);

	setupTimeRows();
	setupButtonRows();

	rows_->addLayout(starttime_row_);
	rows_->addLayout(activity_row_);
	rows_->addLayout(pause_row_);
	rows_->addLayout(timerbutton_row_);
	rows_->addLayout(optionbutton_row_);
	rows_->addLayout(bottombutton_row_);
	

	applyStartupSettingsToGui();
}

void ContentWidget::setupTimeRows()
{
	QFont label_font = QApplication::font();
	label_font.setPixelSize(12);

	// Start Time: --:--
	starttime_row_ = new QHBoxLayout();
	starttime_text_ = new QLabel("Start Time:");
	starttime_text_->setFont(label_font);
	starttime_value_ = new QLabel("--:--");
	starttime_value_->setFont(label_font);
	starttime_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	starttime_row_->addWidget(starttime_text_);
	starttime_row_->addWidget(starttime_value_);

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
	button_font.setPixelSize(11);

	// [START] [STOP]
	timerbutton_row_ = new QHBoxLayout();
	startpause_button_ = new QPushButton("START");
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	startpause_button_->setFixedSize(100,25);
	stop_button_ = new QPushButton("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	stop_button_->setFixedSize(100, 25);
	timerbutton_row_->addWidget(startpause_button_);
	timerbutton_row_->addWidget(stop_button_);

	// [Stay on Top] [Auto-Pause]
	optionbutton_row_ = new QHBoxLayout();
	pintotop_button_ = new QPushButton("Stay on Top");
	pintotop_button_->setFont(button_font);
	pintotop_button_->setToolTip("Keep this Window in Foreground");
	autopause_button_ = new QPushButton("Auto-Pause");
	autopause_button_->setFont(button_font);
	autopause_tooltip1_ = "min after locking the PC:\nPause the Timer and count these ";
	autopause_tooltip2_ = "min retroactively as a Pause";
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip1_ + settings_.getBackpauseMin() + autopause_tooltip2_);
	optionbutton_row_->addWidget(pintotop_button_);
	optionbutton_row_->addWidget(autopause_button_);

	// [Min to Tray] [History..]
	bottombutton_row_ = new QHBoxLayout();
	mintotray_button_ = new QPushButton("Min to Tray");
	mintotray_button_->setFont(button_font);
	mintotray_button_->setToolTip("Minimize to Tray Icon now");
	show_history_button_ = new QPushButton("History..");
	show_history_button_->setFont(button_font);
	show_history_button_->setToolTip("Show History");
	bottombutton_row_->addWidget(mintotray_button_);
	bottombutton_row_->addWidget(show_history_button_);
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
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip1_ + settings_.getBackpauseMin() + autopause_tooltip2_);
}

void ContentWidget::pressedShowHistoryButton()
{
    HistoryDialog dlg(timetracker_, settings_, this);
    dlg.exec();
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
		activity_time_tooltip_base_ = "h overall since " + QTime::currentTime().toString("HH:mm") + " o'clock";
		setActivityTimeTooltip();
		resetPauseTimeTooltip();
		// Update start time when starting from stopped state
		starttime_value_->setText(QTime::currentTime().toString("HH:mm"));
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
	
	// Update start time when starting from stopped state
	if (startpause_button_->text() == "START") {
		starttime_value_->setText(QTime::currentTime().toString("HH:mm"));
	}
}

void ContentWidget::setGUItoStop()
{
	startpause_button_->setText("START");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : black; }");
	starttime_value_->setText("--:--");
}

void ContentWidget::setGUItoPause()
{
	startpause_button_->setText("CONTINUE");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : green; }");
}

void ContentWidget::updateTimes()
{
	qint64 t_active = timetracker_.getActiveTime();
	qint64 t_pause = timetracker_.getPauseTime();
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

bool ContentWidget::isGUIinPause() 
{ 
	return startpause_button_->text() == "CONTINUE"; 
}


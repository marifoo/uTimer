#include "contentwidget.h"
#include <QDateTime>
#include <QtDebug>
#include <QFont>
#include <QColor>
#include <QStringList>

ContentWidget::ContentWidget(Settings & settings, QWidget *parent) : QWidget(parent), settings_(settings)
{
	setupGUI();

	QObject::connect(startpause_button_.get(), SIGNAL(clicked()), this, SLOT(pressedStartPauseButton()));
	QObject::connect(stop_button_.get(), SIGNAL(clicked()), this, SLOT(pressedStopButton()));
	QObject::connect(mintotray_button_.get(), SIGNAL(clicked()), this, SLOT(pressedMinToTrayButton()));
	QObject::connect(pintotop_button_.get(), SIGNAL(clicked()), this, SLOT(pressedPinToTopButton()));
	QObject::connect(autopause_button_.get(), SIGNAL(clicked()), this, SLOT(pressedAutoPauseButton()));
}

void ContentWidget::setupGUI()
{
	// Activity Time:  00:00:00
	activity_row_ = std::make_unique<QHBoxLayout>();
	activity_text_ = std::make_unique<QLabel>("Activity Time:");
	QFont label_font = activity_text_->font();
	label_font.setPointSize(9); // 15px
	activity_text_->setFont(label_font);
	activity_time_ = std::make_unique<QLabel>("00:00:00");
	activity_time_->setFont(label_font);
	activity_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	activity_row_->addWidget(activity_text_.get());
	activity_row_->addWidget(activity_time_.get());

	// Pause Time:  00:00:00
	pause_row_ = std::make_unique<QHBoxLayout>();
	pause_text_ = std::make_unique<QLabel>("Pause Time:");
	pause_text_->setFont(label_font);
	pause_time_ = std::make_unique<QLabel>("00:00:00");
	pause_time_->setFont(label_font);
	pause_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	pause_row_->addWidget(pause_text_.get());
	pause_row_->addWidget(pause_time_.get());

	// [START] [STOP]
	button_row_ = std::make_unique<QHBoxLayout>();
	startpause_button_ = std::make_unique<QPushButton>("START");
	QFont button_font = startpause_button_->font();
	button_font.setPointSize(8); // 13px
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	stop_button_ = std::make_unique<QPushButton>("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	button_row_->addWidget(startpause_button_.get());
	button_row_->addWidget(stop_button_.get());

	// [Min to Tray] [Stay on Top] [Auto-Pause]
	optionbutton_row_ = std::make_unique<QHBoxLayout>();
	mintotray_button_ = std::make_unique<QPushButton>("Min to Tray");
	mintotray_button_->setFont(button_font);
	mintotray_button_->setToolTip("Minimize to Tray Icon now");
	pintotop_button_ = std::make_unique<QPushButton>("Stay on Top");
	pintotop_button_->setFont(button_font);
	pintotop_button_->setToolTip("Keep this Window in Foreground");
	autopause_button_ = std::make_unique<QPushButton>("Auto-Pause");
	autopause_button_->setFont(button_font);
	autopause_tooltip_ = "min after locking the PC via [Win]+[L], convert this whole time into a Pause\nBeware: Locking via [Alt]+[Del] is not detected";
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip_);
	optionbutton_row_->addWidget(mintotray_button_.get());
	optionbutton_row_->addWidget(pintotop_button_.get());
	optionbutton_row_->addWidget(autopause_button_.get());

	rows_ = std::make_unique<QVBoxLayout>();
	rows_->addLayout(activity_row_.get());
	rows_->addLayout(pause_row_.get());
	rows_->addLayout(button_row_.get());
	rows_->addLayout(optionbutton_row_.get());
	setLayout(rows_.get());

	button_hold_color_ = Qt::gray;

	if (settings_.isAutopauseEnabled())
		doButtonColorToggle(autopause_button_, button_hold_color_);

	if (settings_.isPinnedStartEnabled())
		doButtonColorToggle(pintotop_button_, button_hold_color_);
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
	doButtonColorToggle(pintotop_button_, button_hold_color_);
	settings_.setPinToTopState(!settings_.isPinnedStartEnabled());
	emit toggleAlwaysOnTop();
}

void ContentWidget::pressedAutoPauseButton()
{
	doButtonColorToggle(autopause_button_, button_hold_color_);
	settings_.setAutopauseState(!settings_.isAutopauseEnabled());
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip_);
}

void ContentWidget::doButtonColorToggle(std::unique_ptr<QPushButton> &button, QColor color)
{
	const QString stylesheet_string = QString("QPushButton {background-color: %1;}").arg(color.name());
	if (button->styleSheet() != stylesheet_string)
		button->setStyleSheet(stylesheet_string);
	else
		button->setStyleSheet("");
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

void ContentWidget::setGUItoActivity()
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

QString ContentWidget::convertTimeStrToDurationStr(const QString &activity) const
{
	const QStringList split = activity.split(":");

	QString hours = split[0];
	if (hours.startsWith("0")) { hours.remove(0,1); }

	QString minutes = QString::number(static_cast<int>(float(100.0)*(split[1].toFloat())/float(60.0))); //.rightJustified(2, '0');
	if (minutes.length() == 1) { minutes = "0"+minutes; }

	return(hours + "." + minutes);
}

void ContentWidget::setAllTimes(const QString &activity, const QString &pause)
{
	pause_time_->setText(pause);
	activity_time_->setText(activity);
	setActivityTimeTooltip(convertTimeStrToDurationStr(activity));
}

QString ContentWidget::getTooltip()
{
	QString tooltip = "tooltip_error_state_unknown";
	if (startpause_button_->text() == "CONTINUE")
		tooltip = "µTimer:  In Pause (" + pause_time_->text() + ")";
	else if (startpause_button_->text() == "START")
		tooltip = "µTimer:  Timing inactive";
	else if (startpause_button_->text() == "PAUSE")
		tooltip = "µTimer:  In Activity (" + activity_time_->text() + ")";
	return tooltip;
}

bool ContentWidget::isGUIinActivity()
{
	return (startpause_button_->text() == "PAUSE");
}

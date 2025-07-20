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

ContentWidget::ContentWidget(Settings & settings, TimeTracker &timetracker, QWidget *parent) 
	: QWidget(parent), settings_(settings), button_hold_color_(QColor(180,216,228,255)), timetracker_(timetracker)
{
	setupGUI();

	QObject::connect(startpause_button_, SIGNAL(clicked()), this, SLOT(pressedStartPauseButton()));
	QObject::connect(stop_button_, SIGNAL(clicked()), this, SLOT(pressedStopButton()));
	QObject::connect(mintotray_button_, SIGNAL(clicked()), this, SLOT(pressedMinToTrayButton()));
	QObject::connect(pintotop_button_, SIGNAL(clicked()), this, SLOT(pressedPinToTopButton()));
	QObject::connect(autopause_button_, SIGNAL(clicked()), this, SLOT(pressedAutoPauseButton()));
	QObject::connect(show_durations_button_, SIGNAL(clicked()), this, SLOT(pressedShowDurationsButton()));
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

	// [START] [STOP] [SHOW DURATIONS]
	button_row_ = new QHBoxLayout();
	startpause_button_ = new QPushButton("START");
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	stop_button_ = new QPushButton("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	show_durations_button_ = new QPushButton("Show Durations");
	show_durations_button_->setFont(button_font);
	show_durations_button_->setToolTip("Show all durations");
	button_row_->addWidget(startpause_button_);
	button_row_->addWidget(stop_button_);
	button_row_->addWidget(show_durations_button_);

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

void ContentWidget::pressedShowDurationsButton()
{
	QDialog dlg(this);
	dlg.setWindowTitle("Durations");
	QVBoxLayout* layout = new QVBoxLayout(&dlg);

	QString spacer = "  ";

	QTableWidget* table = new QTableWidget(&dlg);
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels({ "Type  ", "Start - End  ", "Duration  ", "Activity  " });
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Type
	table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch); // Start - End
	table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents); // Duration
	table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents); // Activity
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionMode(QAbstractItemView::NoSelection);

	struct PendingChange {
		size_t row;
		DurationType newType;
	};
	std::vector<PendingChange> pendingChanges;

	auto& durations = timetracker_.getDurations();
	table->setRowCount(static_cast<int>(durations.size()));
	int row = 0;
	for (const auto& d : durations) {
		QString typeStr = (d.type == DurationType::Activity) ? "Activity" + spacer : "Pause" + spacer;
		table->setItem(row, 0, new QTableWidgetItem(typeStr));

		QString startEndStr = d.endTime.addMSecs(-d.duration).toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss");
		table->setItem(row, 1, new QTableWidgetItem(startEndStr));

		QString durationStr = convMSecToTimeStr(d.duration) + spacer;
		table->setItem(row, 2, new QTableWidgetItem(durationStr));

		QCheckBox* box = new QCheckBox(table);
		box->setChecked(d.type == DurationType::Activity);
		table->setCellWidget(row, 3, box);

		QObject::connect(box, &QCheckBox::stateChanged, [&pendingChanges, table, row, spacer](int state) {
			DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
			QString typeStr = (newType == DurationType::Activity) ? "Activity *  " : "Pause *  ";
			table->item(row, 0)->setText(typeStr);

			auto it = std::find_if(pendingChanges.begin(), pendingChanges.end(),
				[row](const PendingChange& pc) { return pc.row == row; });
			if (it != pendingChanges.end()) {
				it->newType = newType;
			} else {
				pendingChanges.push_back({static_cast<size_t>(row), newType});
			}
		});
		++row;
	}

	QHBoxLayout* buttonLayout = new QHBoxLayout();
	QPushButton* okButton = new QPushButton("OK", &dlg);
	QPushButton* cancelButton = new QPushButton("Cancel", &dlg);
	buttonLayout->addStretch();
	buttonLayout->addWidget(okButton);
	buttonLayout->addWidget(cancelButton);

	layout->addWidget(table);
	layout->addLayout(buttonLayout);
	dlg.setLayout(layout);
	dlg.resize(400, 400);

	QObject::connect(okButton, &QPushButton::clicked, &dlg, &QDialog::accept);
	QObject::connect(cancelButton, &QPushButton::clicked, &dlg, &QDialog::reject);
	
	if (dlg.exec() == QDialog::Accepted) {
		for (const auto& change : pendingChanges) {
			timetracker_.setDurationType(change.row, change.newType);
		}
	}
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


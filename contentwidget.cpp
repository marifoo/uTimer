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
	QObject::connect(show_history_button_, SIGNAL(clicked()), this, SLOT(pressedShowHistoryButton()));
}

void ContentWidget::setupGUI()
{
	rows_ = new QVBoxLayout(this);

	setupTimeRows();
	setupButtonRows();

	rows_->addLayout(activity_row_);
	rows_->addLayout(pause_row_);
	rows_->addLayout(timerbutton_row_);
	rows_->addLayout(historybutton_row_);
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
	timerbutton_row_ = new QHBoxLayout();
	startpause_button_ = new QPushButton("START");
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	stop_button_ = new QPushButton("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	timerbutton_row_->addWidget(startpause_button_);
	timerbutton_row_->addWidget(stop_button_);

	// [History..]
	historybutton_row_ = new QHBoxLayout();
	show_history_button_ = new QPushButton("History..");
	show_history_button_->setFont(button_font);
	show_history_button_->setToolTip("Show History");
	historybutton_row_->addWidget(show_history_button_);

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
	autopause_tooltip1_ = "min after locking the PC:\nPause the Timer and count these ";
	autopause_tooltip2_ = "min retroactively as a Pause";
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip1_ + settings_.getBackpauseMin() + autopause_tooltip2_);
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
	autopause_button_->setToolTip(settings_.getBackpauseMin() + autopause_tooltip1_ + settings_.getBackpauseMin() + autopause_tooltip2_);
}

void ContentWidget::pressedShowHistoryButton()
{
	auto currentDurations = timetracker_.getCurrentDurations();
	auto historyDurations = timetracker_.getDurationsHistory();

	QMap<QDate, std::vector<TimeDuration>> historyByDay;
	for (const auto& d : historyDurations) {
		historyByDay[d.endTime.date()].push_back(d);
	}
	QList<QDate> historyDates = historyByDay.keys();
	std::sort(historyDates.begin(), historyDates.end());

	auto calculateTotals = [](const std::vector<DurationType>& types, const std::vector<TimeDuration>& durations) {
		qint64 totalActivity = 0;
		qint64 totalPause = 0;
		for (size_t i = 0; i < durations.size(); ++i) {
			if (types[i] == DurationType::Activity)
				totalActivity += durations[i].duration;
			else
				totalPause += durations[i].duration;
		}
		return std::make_pair(totalActivity, totalPause);
	};

	struct Page {
		QString title;
		std::vector<TimeDuration> durations;
		bool isCurrent;
	};
	std::vector<Page> pages;

	pages.push_back({
		QString("Current Session (entries: ") + QString::number(currentDurations.size()) + QString(")"),
		std::vector<TimeDuration>(currentDurations.begin(), currentDurations.end()),
		true
	});

	for (const QDate& date : historyDates) {
		pages.push_back({
			date.toString("yyyy-MM-dd") + QString(" (entries: ") + QString::number(historyByDay[date].size()) + QString(")"),
			historyByDay[date],
			false
		});
	}

    int pageIndex = 0;
    std::vector<std::vector<DurationType>> edits(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        edits[i].resize(pages[i].durations.size());
        for (size_t j = 0; j < pages[i].durations.size(); ++j) {
            edits[i][j] = pages[i].durations[j].type;
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle("History");
    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    QLabel* pageLabel = new QLabel(&dlg);
    layout->addWidget(pageLabel);
    QTableWidget* table = new QTableWidget(&dlg);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({ "Type      ", "Start - End   ", "Duration   ", "Activity   " });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(table);
    QHBoxLayout* navLayout = new QHBoxLayout();
    QPushButton* prevButton = new QPushButton("Previous", &dlg);
    QPushButton* nextButton = new QPushButton("Next", &dlg);
    navLayout->addWidget(prevButton);
    navLayout->addWidget(nextButton);
    layout->addLayout(navLayout);
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* okButton = new QPushButton("OK", &dlg);
    QPushButton* cancelButton = new QPushButton("Cancel", &dlg);
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);
    dlg.setLayout(layout);
    dlg.resize(500, 400);

    auto updateTotalsLabel = [&](int idx) {
        auto totals = calculateTotals(edits[idx], pages[idx].durations);
        QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totals.first) + QString("  Pause: ") + convMSecToTimeStr(totals.second);
        pageLabel->setText(pages[idx].title + totalsStr);
    };

    auto updateTable = [&](int idx) {
        table->clearContents();
        table->setRowCount(static_cast<int>(pages[idx].durations.size()));
        updateTotalsLabel(idx);
        for (int row = 0; row < int(pages[idx].durations.size()); ++row) {
            const auto& d = pages[idx].durations[row];
            QString typeStr = (edits[idx][row] == DurationType::Activity) ? "Activity    " : "Pause    ";
            table->setItem(row, 0, new QTableWidgetItem(typeStr));
            QString startEndStr = d.endTime.addMSecs(-d.duration).toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss");
            table->setItem(row, 1, new QTableWidgetItem(startEndStr));
            QString durationStr = convMSecToTimeStr(d.duration) + "  ";
            table->setItem(row, 2, new QTableWidgetItem(durationStr));
            QCheckBox* box = new QCheckBox(table);
            box->setChecked(edits[idx][row] == DurationType::Activity);
            table->setCellWidget(row, 3, box);
            QObject::connect(box, &QCheckBox::stateChanged, [&, idx, row, box](int state) {
                edits[idx][row] = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
                QString typeStr = (edits[idx][row] == DurationType::Activity) ? "Activity *  " : "Pause *  ";
                table->item(row, 0)->setText(typeStr);
                updateTotalsLabel(idx);
            });
        }
        nextButton->setEnabled(idx > 0);
        prevButton->setEnabled(idx < int(pages.size()) - 1);
    };

    updateTable(pageIndex);

    QObject::connect(prevButton, &QPushButton::clicked, [&]() {
        if (pageIndex < int(pages.size()) - 1) {
            pageIndex++;
            updateTable(pageIndex);
        }
    });
    QObject::connect(nextButton, &QPushButton::clicked, [&]() {
        if (pageIndex > 0) {
            pageIndex--;
            updateTable(pageIndex);
        }
    });
    QObject::connect(okButton, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelButton, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        for (size_t i = 0; i < pages.size(); ++i) {
            for (size_t j = 0; j < pages[i].durations.size(); ++j) {
                if (pages[i].isCurrent) {
                    timetracker_.setDurationType(j, edits[i][j]);
                } else {
                    pages[i].durations[j].type = edits[i][j];
                }
            }
        }
        std::deque<TimeDuration> allHistory;
        for (size_t i = 1; i < pages.size(); ++i) {
            allHistory.insert(allHistory.end(), pages[i].durations.begin(), pages[i].durations.end());
        }
        if (!allHistory.empty()) {
            timetracker_.replaceDurationsInDB(allHistory);
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


#include "historydialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QPushButton>
#include <QMenu>
#include <QMessageBox>
#include <QApplication>
#include "helpers.h"
#include "logger.h"
#include <QSlider>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QButtonGroup>

HistoryDialog::HistoryDialog(TimeTracker& timetracker, const Settings& settings, QWidget* parent)
    : QDialog(parent), timetracker_(timetracker), settings_(settings), pageIndex_(0)
{
    createPages();
    setupUI();
    updateTable(pageIndex_);
    
    // Enable right-click context menu for split functionality
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &HistoryDialog::showContextMenu);

    // Log initial state for debugging
    if (settings_.logToFile()) {
        Logger::Log("[HISTORY] Dialog opened");
        
        auto historyDurations = timetracker_.getDurationsHistory();
        Logger::Log(QString("[HISTORY] Loaded %1 total durations from DB").arg(historyDurations.size()));

        for (size_t i = 0; i < pages_.size(); ++i) {
            Logger::Log(QString("[HISTORY] Page %1 - Title: %2, Entries: %3, IsCurrent: %4")
                .arg(i)
                .arg(pages_[i].title)
                .arg(pages_[i].durations.size())
                .arg(pages_[i].isCurrent));
        }
    }
}

void HistoryDialog::createPages()
{
    auto currentDurations = timetracker_.getCurrentDurations();
    auto historyDurations = timetracker_.getDurationsHistory();

    // Group historical durations by date
    QMap<QDate, std::deque<TimeDuration>> historyByDay;
    for (const auto& d : historyDurations) {
        historyByDay[d.endTime.date()].push_back(d);
    }
    QList<QDate> historyDates = historyByDay.keys();
    std::sort(historyDates.begin(), historyDates.end(), std::greater<QDate>()); // Most recent first

    // Add current session as first page
    pages_.push_back({
        QString("Current Session (entries: ") + QString::number(currentDurations.size()) + QString(")"),
        currentDurations,
        true
    });

    // Add historical pages (one per day)
    for (const QDate& date : historyDates) {
        pages_.push_back({
            date.toString("yyyy-MM-dd") + QString(" (entries: ") + QString::number(historyByDay[date].size()) + QString(")"),
            historyByDay[date],
            false
        });
    }

    // Initialize working copy for pending changes
    pendingChanges_.resize(pages_.size());
    for (size_t i = 0; i < pages_.size(); ++i) {
        pendingChanges_[i] = pages_[i].durations;
    }
}

void HistoryDialog::setupUI()
{
    setWindowTitle("History");
    QVBoxLayout* layout = new QVBoxLayout(this);

    // Page title and totals label
    pageLabel_ = new QLabel(this);
    layout->addWidget(pageLabel_);

    // Main table showing duration entries
    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({ "Type      ", "Start - End   ", "Duration   ", "Activity   " });
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_);

    // Navigation buttons for page switching
    QHBoxLayout* navLayout = new QHBoxLayout();
    prevButton_ = new QPushButton("Previous", this);
    nextButton_ = new QPushButton("Next", this);
    navLayout->addWidget(prevButton_);
    navLayout->addWidget(nextButton_);
    layout->addLayout(navLayout);

    // Dialog control buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* okButton = new QPushButton("OK", this);
    QPushButton* cancelButton = new QPushButton("Cancel", this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

    // Connect signals
    connect(prevButton_, &QPushButton::clicked, this, &HistoryDialog::onPrevClicked);
    connect(nextButton_, &QPushButton::clicked, this, &HistoryDialog::onNextClicked);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    resize(400, 400);
}

std::pair<qint64, qint64> HistoryDialog::calculateTotals(const std::deque<TimeDuration>& durations)
{
    qint64 totalActivity = 0;
    qint64 totalPause = 0;
    for (const auto& duration : durations) {
        if (duration.type == DurationType::Activity)
            totalActivity += duration.duration;
        else
            totalPause += duration.duration;
    }
    return std::make_pair(totalActivity, totalPause);
}

void HistoryDialog::updateTotalsLabel(uint idx)
{
    if (idx >= pendingChanges_.size() || idx >= pages_.size()) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[HISTORY] Error: updateTotalsLabel: Invalid index %1").arg(idx));
        }
        return;
    }

    auto totals = calculateTotals(pendingChanges_[idx]);
    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totals.first) + QString("  Pause: ") + convMSecToTimeStr(totals.second);
    pageLabel_->setText(pages_[idx].title + totalsStr);
}

void HistoryDialog::updateTable(uint idx)
{
    if (idx >= pendingChanges_.size() || idx >= pages_.size()) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[HISTORY] Error: updateTable: Invalid index %1").arg(idx));
        }
        return;
    }

    // Clean up existing checkboxes to prevent memory leaks
    for (int row = 0; row < table_->rowCount(); ++row) {
        QWidget* cellWidget = table_->cellWidget(row, 3);
        if (cellWidget) {
            cellWidget->disconnect(); // Prevent orphaned signal connections
            table_->removeCellWidget(row, 3);
        }
    }

    table_->clearContents();
    table_->setRowCount(static_cast<int>(pendingChanges_[idx].size()));
    updateTotalsLabel(idx);

    // Determine if this page has been modified
    bool pageModified = false;
    if (pendingChanges_[idx].size() != pages_[idx].durations.size()) {
        pageModified = true;
    } else {
        for (size_t i = 0; i < pendingChanges_[idx].size(); ++i) {
            const auto& pending = pendingChanges_[idx][i];
            const auto& original = pages_[idx].durations[i];
            if (pending.type != original.type || 
                pending.duration != original.duration || 
                pending.endTime != original.endTime) {
                pageModified = true;
                break;
            }
        }
    }

    // Populate table rows
    for (int row = 0; row < int(pendingChanges_[idx].size()); ++row) {
        const auto& d = pendingChanges_[idx][row];
        QString typeStr = d.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(d.endTime.addMSecs(-d.duration).toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        // Add checkbox for Activity/Pause toggle
        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(d.type == DurationType::Activity);
        table_->setCellWidget(row, 3, box);

        // Handle checkbox state changes with proper validation
        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, capturedRow = row](int state) {
            // Reject stale events from checkboxes on other pages
            if (capturedPageIdx != pageIndex_) {
                return;
            }

            if (capturedRow >= static_cast<int>(pendingChanges_[capturedPageIdx].size())) {
                if (settings_.logToFile()) {
                    Logger::Log(QString("[WARNING] Checkbox callback: Invalid row %1").arg(capturedRow));
                }
                return;
            }

            // Update the duration type in pending changes
            pendingChanges_[capturedPageIdx][capturedRow].type = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            QString typeStr = pendingChanges_[capturedPageIdx][capturedRow].type == DurationType::Activity ? "Activity  " : "Pause  ";

            // Update UI
            if (table_->item(capturedRow, 0)) {
                table_->item(capturedRow, 0)->setText(typeStr);
                updateTotalsLabel(capturedPageIdx);

                // Highlight the modified row
                for (int col = 0; col < table_->columnCount(); ++col) {
                    if (table_->item(capturedRow, col)) {
                        table_->item(capturedRow, col)->setBackground(QColor(180, 216, 228, 255));
                    }
                }
            }
        });

        // Highlight all rows if page has been modified
        if (pageModified) {
            for (int col = 0; col < table_->columnCount(); ++col) {
                if (table_->item(row, col)) {
                    table_->item(row, col)->setBackground(QColor(180, 216, 228, 255));
                }
            }
        }
    }

    // Update navigation button states
    prevButton_->setEnabled(pageIndex_ < pages_.size() - 1);
    nextButton_->setEnabled(pageIndex_ > 0);
}

void HistoryDialog::onPrevClicked()
{
    if (pageIndex_ < pages_.size() - 1) {
        pageIndex_++;
        updateTable(pageIndex_);
    }
}

void HistoryDialog::onNextClicked()
{
    if (pageIndex_ > 0) {
        pageIndex_--;
        updateTable(pageIndex_);
    }
}

void HistoryDialog::saveChanges()
{
    if (result() != QDialog::Accepted) {
        if (settings_.logToFile()) {
            Logger::Log("[HISTORY] Dialog cancelled, discarding changes");
        }
        return;
    }

    // Apply all pending changes to the original pages
    for (size_t i = 0; i < pages_.size(); ++i) {
        std::swap(pages_[i].durations, pendingChanges_[i]);
    }

    // Update TimeTracker's current session (in-memory only)
    // Collect ONLY historical durations for DB
    std::deque<TimeDuration> historyDurations;

    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].isCurrent) {
            // Update in-memory current session - NOT saved to DB yet
            timetracker_.setCurrentDurations(pages_[i].durations);
            if (settings_.logToFile()) {
                Logger::Log("[HISTORY] Updated TimeTracker current session (in-memory)");
            }
        } else {
            // Collect historical durations for DB save
            historyDurations.insert(historyDurations.end(),
                                   pages_[i].durations.begin(),
                                   pages_[i].durations.end());
        }
    }

    // Save ONLY historical durations to DB
    if (!historyDurations.empty()) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[HISTORY] Saving %1 historical durations to DB").arg(historyDurations.size()));
        }

        bool success = timetracker_.replaceDurationsInDB(historyDurations);
        if (!success) {
            if (settings_.logToFile()) {
                Logger::Log("[HISTORY] CRITICAL: Failed to save durations to DB");
            }
            QMessageBox::critical(this, "Database Error",
                "Failed to save changes to the database. Your changes to historical entries may be lost.");
        } else if (settings_.logToFile()) {
            Logger::Log("[HISTORY] Successfully saved historical durations to DB");
        }
    } else if (settings_.logToFile()) {
        Logger::Log("[HISTORY] No historical durations to save");
    }
}

void HistoryDialog::showContextMenu(const QPoint& pos)
{
    int row = table_->rowAt(pos.y());
    if (row < 0) return;
    
    contextMenuRow_ = row;
    QMenu menu(this);
    QAction* splitAction = menu.addAction("Split..");
    connect(splitAction, &QAction::triggered, this, &HistoryDialog::onSplitRow);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void HistoryDialog::onSplitRow()
{
    if (contextMenuRow_ < 0) return;

    // Capture and reset row immediately to prevent stale state (Issue #5)
    int row = contextMenuRow_;
    contextMenuRow_ = -1;

    uint idx = pageIndex_;

    if (idx >= pendingChanges_.size() || row >= static_cast<int>(pendingChanges_[idx].size())) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[HISTORY] Error: onSplitRow: Invalid indices - page: %1, row: %2").arg(idx).arg(row));
        }
        return;
    }

    TimeDuration& duration = pendingChanges_[idx][row];
    qint64 originalDuration = duration.duration;
    QDateTime start = duration.endTime.addMSecs(-originalDuration);
    QDateTime end = duration.endTime;

    // Check if duration is long enough to split meaningfully (at least 2 seconds)
    if (start.secsTo(end) <= 2) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[HISTORY] Error: onSplitRow: Duration too short to split - only %1 seconds")
                .arg(start.secsTo(end)));
        }
        QMessageBox::information(this, "Split Duration",
            "This duration is too short to split meaningfully (minimum 3 seconds required).");
        return;
    }

    // Show split dialog
    SplitDialog dlg(start, end, this);
    if (dlg.exec() == QDialog::Accepted) {
        QDateTime splitTime = dlg.getSplitTime();
        qint64 firstDuration = start.msecsTo(splitTime);
        qint64 secondDuration = splitTime.msecsTo(end);

        // Validate split preserves total duration (Issue #4)
        if (firstDuration + secondDuration != originalDuration) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[HISTORY] Warning: Split duration mismatch - original: %1ms, split sum: %2ms, adjusting")
                    .arg(originalDuration).arg(firstDuration + secondDuration));
            }
            // Adjust second segment to preserve total duration
            secondDuration = originalDuration - firstDuration;
        }

        // Validate split results - ensure both segments have at least 1 second
        if (firstDuration < 1000 || secondDuration < 1000) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[HISTORY] Error: onSplitRow: Split results too small - first: %1ms, second: %2ms")
                    .arg(firstDuration).arg(secondDuration));
            }
            QMessageBox::warning(this, "Split Duration",
                "Invalid split: both segments must be at least 1 second long.");
            return;
        }

        DurationType firstType = dlg.getFirstSegmentType();
        DurationType secondType = dlg.getSecondSegmentType();

        TimeDuration first(firstType, firstDuration, splitTime);
        TimeDuration second(secondType, secondDuration, end);

        // Replace original duration with two new segments using index-based operations
        // (Avoid iterator invalidation issues with deque erase/insert)
        auto& durationsDeque = pendingChanges_[idx];
        durationsDeque.erase(durationsDeque.begin() + row);
        durationsDeque.insert(durationsDeque.begin() + row, first);
        durationsDeque.insert(durationsDeque.begin() + row + 1, second);
       
        updateTable(idx);
    }
}

// === SplitDialog Implementation ===

SplitDialog::SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent)
    : QDialog(parent), start_(start), end_(end)
{
    setWindowTitle("Split Duration");
    QVBoxLayout* layout = new QVBoxLayout(this);

    // Time range display with slider
    QHBoxLayout* rowLayout = new QHBoxLayout();
    QLabel* startLabel = new QLabel(QString("Start: %1").arg(start.toString("hh:mm:ss")), this);
    QLabel* endLabel = new QLabel(QString("End: %1").arg(end.toString("hh:mm:ss")), this);
    slider_ = new QSlider(Qt::Horizontal, this);
    
    int totalSecs = start.secsTo(end);
    if (totalSecs <= 2) {
        // Duration too short to split meaningfully
        slider_->setMinimum(1);
        slider_->setMaximum(1);
        slider_->setValue(1);
        slider_->setEnabled(false);
    } else {
        slider_->setMinimum(1);
        slider_->setMaximum(totalSecs - 1);
        slider_->setValue(totalSecs / 2); // Start at midpoint
    }

    rowLayout->addWidget(startLabel);
    rowLayout->addWidget(slider_);
    rowLayout->addWidget(endLabel);
    layout->addLayout(rowLayout);

    // Split time display
    splitTimeLabel_ = new QLabel(this);
    splitTimeLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(splitTimeLabel_);
    updateSplitLabel(slider_->value());
    connect(slider_, &QSlider::valueChanged, this, &SplitDialog::updateSplitLabel);

    // Segment type selection - simplified to 2 radio buttons
    QVBoxLayout* segmentTypeLayout = new QVBoxLayout();
    QLabel* segmentTypeLabel = new QLabel("Split Type:", this);
    layout->addWidget(segmentTypeLabel);

    activityPauseOption_ = new QRadioButton("First: Activity, Second: Pause", this);
    pauseActivityOption_ = new QRadioButton("First: Pause, Second: Activity", this);
    
    // Default to "Pause -> Activity" as requested
    pauseActivityOption_->setChecked(true);

    QButtonGroup* segmentGroup = new QButtonGroup(this);
    segmentGroup->addButton(activityPauseOption_);
    segmentGroup->addButton(pauseActivityOption_);

    segmentTypeLayout->addWidget(activityPauseOption_);
    segmentTypeLayout->addWidget(pauseActivityOption_);
    layout->addLayout(segmentTypeLayout);

    // Dialog buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    
    // Ensure minimum dialog size for very short durations
    resize(450, std::max(height(), 120));
}

void SplitDialog::updateSplitLabel(int value)
{
    QDateTime split = start_.addSecs(value);
    splitTimeLabel_->setText(QString("Split Time: %1").arg(split.toString("hh:mm:ss")));
}

QDateTime SplitDialog::getSplitTime() const
{
    return start_.addSecs(slider_->value());
}

DurationType SplitDialog::getFirstSegmentType() const
{
    return activityPauseOption_->isChecked() ? DurationType::Activity : DurationType::Pause;
}

DurationType SplitDialog::getSecondSegmentType() const
{
    return activityPauseOption_->isChecked() ? DurationType::Pause : DurationType::Activity;
}

void SplitDialog::setFirstSegmentType(DurationType type)
{
    if (type == DurationType::Activity) {
        activityPauseOption_->setChecked(true);
    } else {
        pauseActivityOption_->setChecked(true);
    }
}

void SplitDialog::setSecondSegmentType(DurationType type)
{
    // Since we now have only 2 mutually exclusive options, 
    // setting the second segment type determines the first as well
    if (type == DurationType::Pause) {
        activityPauseOption_->setChecked(true);  // Activity -> Pause
    } else {
        pauseActivityOption_->setChecked(true);  // Pause -> Activity
    }
}

HistoryDialog::~HistoryDialog() {
    if (settings_.logToFile()) {
        Logger::Log("[HISTORY] Dialog closing");
    }
    
    // Clean up cell widgets to prevent memory leaks
    if (table_) {
        for (int row = 0; row < table_->rowCount(); ++row) {
            QWidget* cellWidget = table_->cellWidget(row, 3);
            if (cellWidget) {
                cellWidget->disconnect();
                table_->removeCellWidget(row, 3);
            }
        }
    }
    
    pendingChanges_.clear();
}
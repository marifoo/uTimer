/**
 * HistoryDialog - Interface for viewing and editing past time logs.
 *
 * Architecture:
 * - Transactional Editing: All changes (type toggling, splitting) are performed
 *   on local copies (`pendingTimelines_`). The actual database and TimeTracker
 *   are only updated when the user clicks "OK".
 * - Paging Model: Data is grouped by Date (Pages). Page 0 is always "Today"
 *   (in-memory + today's DB + ongoing), while subsequent pages are historical (DB).
 * - Safety: Checkpoints are paused while this dialog is open to prevent
 *   database race conditions or state inconsistencies during editing.
 */

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
#include <QFileInfo>
#include "helpers.h"
#include "logger.h"
#include <QSlider>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QButtonGroup>
#include <algorithm>

namespace {
constexpr int kMinimumSplitDurationSeconds = 3;

bool isSameSegmentId(const TimeDuration& a, const TimeDuration& b)
{
    if (a.segment_id.isEmpty() || b.segment_id.isEmpty()) {
        return false;
    }
    return a.segment_id == b.segment_id;
}
}

HistoryDialog::HistoryDialog(TimeTracker& timetracker, const Settings& settings, QWidget* parent)
    : QDialog(parent), timetracker_(timetracker), settings_(settings), pageIndex_(0)
{
    // Pause checkpoints while dialog is open to prevent race conditions
    timetracker_.pauseCheckpoints();

    createPages();
    setupUI();
    updateTable(pageIndex_);
    
    // Enable right-click context menu for split functionality
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &HistoryDialog::showContextMenu);

    // Log initial state for debugging
    Logger::Log("[HISTORY] Dialog opened");
    {
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

/**
 * Organizes raw time entries into a paged structure for display.
 *
 * Structure:
 * - Page 0: "Today" (in-memory + today's DB + ongoing).
 * - Page 1..N: Historical sessions loaded from the database, grouped by Date.
 *
 * This keeps all sessions for the current day in one place, while older days
 * remain separate.
 */
void HistoryDialog::createPages()
{
    Timeline sessionSnapshot = timetracker_.snapshot();
    const auto& currentDurations = sessionSnapshot.completed();
    const auto& ongoingOpt = sessionSnapshot.ongoing();
    auto historyDurations = timetracker_.getDurationsHistory();
    const QDate today = QDate::currentDate();

    std::vector<TimeDuration> currentComparableDurations;
    currentComparableDurations.reserve(currentDurations.size() + (ongoingOpt.has_value() ? 1 : 0));
    currentComparableDurations.insert(currentComparableDurations.end(), currentDurations.begin(), currentDurations.end());
    if (ongoingOpt.has_value()) {
        currentComparableDurations.push_back(ongoingOpt.value());
    }

    // Group historical durations by date
    QMap<QDate, std::deque<TimeDuration>> historyByDay;
    for (const auto& d : historyDurations) {
        const bool isDuplicateOfCurrent = std::any_of(
            currentComparableDurations.begin(),
            currentComparableDurations.end(),
            [&d](const TimeDuration& currentDuration) {
                return isSameSegmentId(d, currentDuration);
            }
        );

        if (isDuplicateOfCurrent) {
            continue;
        }
        historyByDay[d.startTime.date()].push_back(d);
    }
    QList<QDate> historyDates = historyByDay.keys();
    std::sort(historyDates.begin(), historyDates.end(), std::greater<QDate>()); // Most recent first

    std::deque<TimeDuration> currentPageCompleted;
    std::vector<bool> currentPageIsMemory;
    currentPageCompleted.insert(currentPageCompleted.end(), currentDurations.begin(), currentDurations.end());
    currentPageIsMemory.insert(currentPageIsMemory.end(), currentDurations.size(), true);

    auto todayIt = historyByDay.find(today);
    if (todayIt != historyByDay.end()) {
        currentPageCompleted.insert(currentPageCompleted.end(), todayIt->begin(), todayIt->end());
        currentPageIsMemory.insert(currentPageIsMemory.end(), todayIt->size(), false);
    }

    // Build currentPageDurations for the Page struct (includes ongoing for title count)
    std::deque<TimeDuration> currentPageDurations = currentPageCompleted;
    if (ongoingOpt.has_value()) {
        currentPageDurations.push_back(ongoingOpt.value());
    }

    // Add today page (current session + today's DB + ongoing)
    pages_.push_back({
        QString("Today (entries: ") + QString::number(currentPageDurations.size()) + QString(")"),
        currentPageDurations,
        true
    });

    // Add historical pages (one per day)
    for (const QDate& date : historyDates) {
        if (date == today) {
            continue;
        }
        pages_.push_back({
            date.toString("yyyy-MM-dd") + QString(" (entries: ") + QString::number(historyByDay[date].size()) + QString(")"),
            historyByDay[date],
            false
        });
    }

    // Initialize pendingTimelines_ and isMemoryRow_
    pendingTimelines_.reserve(pages_.size());
    isMemoryRow_.reserve(pages_.size());

    // Page 0: completed = currentPageCompleted (no ongoing), ongoing = sessionSnapshot.ongoing()
    pendingTimelines_.push_back(Timeline(currentPageCompleted, ongoingOpt));
    isMemoryRow_.push_back(currentPageIsMemory);

    for (size_t i = 1; i < pages_.size(); ++i) {
        pendingTimelines_.push_back(Timeline(pages_[i].durations, std::nullopt));
        isMemoryRow_.push_back(std::vector<bool>(pages_[i].durations.size(), false));
    }

    assertPendingOriginsInvariant();
}

void HistoryDialog::setupUI()
{
    setWindowTitle("History");
    QVBoxLayout* layout = new QVBoxLayout(this);

    // Page title and totals label
    pageLabel_ = new QLabel(this);
    layout->addWidget(pageLabel_);

    loadReconciliationLabel_ = new QLabel(this);
    loadReconciliationLabel_->setWordWrap(true);
    loadReconciliationLabel_->setStyleSheet("QLabel { color: #9A6B00; }");
    // Rich text enables the clickable log-file link embedded by buildLoadReconciliationMessage().
    // setOpenExternalLinks delegates the click to QDesktopServices::openUrl(), which
    // hands "file:///" URLs to the OS file manager without extra plumbing.
    loadReconciliationLabel_->setTextFormat(Qt::RichText);
    loadReconciliationLabel_->setOpenExternalLinks(true);
    const QString reconciliationMessage = buildLoadReconciliationMessage();
    loadReconciliationLabel_->setText(reconciliationMessage);
    loadReconciliationLabel_->setVisible(!reconciliationMessage.isEmpty());
    layout->addWidget(loadReconciliationLabel_);

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
    connect(prevButton_, &QPushButton::clicked, this, &HistoryDialog::onOlder);
    connect(nextButton_, &QPushButton::clicked, this, &HistoryDialog::onNewer);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    resize(400, 400);
}

/**
 * Builds the banner message shown when loadDurations encountered corrupt rows.
 *
 * If debug logging is enabled and the log file exists on disk, appends an HTML
 * anchor so the user can click through to the log for details. The label is
 * configured for Rich Text and external-link handling in setupUI(), so the
 * anchor is opened by the OS file manager / text editor automatically.
 */
QString HistoryDialog::buildLoadReconciliationMessage() const
{
    const auto [skipped, repaired] = timetracker_.getLastHistoryLoadStats();
    if (skipped <= 0 && repaired <= 0) {
        return QString();
    }

    QString message = QString("%1 rows skipped due to corrupt data, %2 rows auto-repaired.")
        .arg(skipped)
        .arg(repaired);

    // Only show a link to the log file when logging is actually enabled; showing it
    // when logging is off is misleading (the file may be stale or absent).
    if (settings_.logToFile()) {
        const QString logPath = Logger::logFilePath();
        if (QFileInfo::exists(logPath)) {
            message += QString("<br><a href=\"file:///%1\">Open log file</a>")
                .arg(logPath);
        }
    }

    return message;
}

QString HistoryDialog::getLoadReconciliationMessage() const
{
    return buildLoadReconciliationMessage();
}

void HistoryDialog::assertPendingOriginsInvariant() const
{
#ifndef QT_NO_DEBUG
    Q_ASSERT_X(isMemoryRow_.size() == pendingTimelines_.size(),
        "HistoryDialog::assertPendingOriginsInvariant",
        "isMemoryRow_ and pendingTimelines_ must have same page count");
    for (size_t i = 0; i < pendingTimelines_.size(); ++i) {
        Q_ASSERT_X(isMemoryRow_[i].size() == pendingTimelines_[i].completed().size(),
            "HistoryDialog::assertPendingOriginsInvariant",
            "isMemoryRow_[i] must have same row count as pendingTimelines_[i].completed()");
    }
#endif
}

void HistoryDialog::updateTotalsLabel(uint idx)
{
    if (idx >= pendingTimelines_.size() || idx >= pages_.size()) {
        Logger::Log(QString("[HISTORY] Error: updateTotalsLabel: Invalid index %1").arg(idx));
        return;
    }

    qint64 totalActivity = pendingTimelines_[idx].activeMsec();
    qint64 totalPause = pendingTimelines_[idx].pauseMsec();
    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totalActivity) + QString("  Pause: ") + convMSecToTimeStr(totalPause);
    pageLabel_->setText(pages_[idx].title + totalsStr);
}

/**
 * Refreshes the UI table with data from the current page.
 *
 * Responsibilities:
 * 1. Clears old data and widgets.
 * 2. Compares pendingChanges against original data to detect modifications.
 * 3. Re-populates the table.
 * 4. Binds Checkboxes for Type Toggling (Activity <-> Pause).
 * 5. Applies visual highlighting if the page has unsaved changes.
 */
void HistoryDialog::updateTable(uint idx)
{
    if (idx >= pendingTimelines_.size() || idx >= pages_.size()) {
        Logger::Log(QString("[HISTORY] Error: updateTable: Invalid index %1").arg(idx));
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

    const auto& comp = pendingTimelines_[idx].completed();
    const bool hasOngoing = pendingTimelines_[idx].ongoing().has_value();
    int rowCount = static_cast<int>(comp.size());
    if (hasOngoing) ++rowCount;
    table_->setRowCount(rowCount);

    updateTotalsLabel(idx);

    // Determine if this page has been modified (compare completed portion only)
    const auto& originalDurations = pages_[idx].durations;
    // For page 0, original durations includes the ongoing row at the end; compare only completed
    size_t originalCompletedSize = originalDurations.size();
    if (idx == 0 && pendingTimelines_[0].ongoing().has_value()) {
        originalCompletedSize = originalDurations.size() > 0 ? originalDurations.size() - 1 : 0;
    }
    bool pageModified = false;
    if (comp.size() != originalCompletedSize) {
        pageModified = true;
    } else {
        for (size_t i = 0; i < comp.size(); ++i) {
            const auto& pending = comp[i];
            const auto& original = originalDurations[i];
            if (pending.type != original.type ||
                pending.duration != original.duration ||
                pending.startTime != original.startTime ||
                pending.endTime != original.endTime) {
                pageModified = true;
                break;
            }
        }
    }

    // Populate completed rows
    for (int row = 0; row < static_cast<int>(comp.size()); ++row) {
        const auto& d = comp[row];
        QString typeStr = d.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(d.type == DurationType::Activity);
        box->setEnabled(true);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, capturedRow = row](int state) {
            // Reject stale events from checkboxes on other pages
            if (capturedPageIdx != pageIndex_) {
                return;
            }

            if (capturedRow >= static_cast<int>(pendingTimelines_[capturedPageIdx].completed().size())) {
                Logger::Log(QString("[WARNING] Checkbox callback: Invalid row %1").arg(capturedRow));
                return;
            }

            DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            pendingTimelines_[capturedPageIdx] = pendingTimelines_[capturedPageIdx].withSegmentType(
                static_cast<size_t>(capturedRow), newType);
            assertPendingOriginsInvariant();
            QString typeStr = newType == DurationType::Activity ? "Activity  " : "Pause  ";

            if (table_->item(capturedRow, 0)) {
                table_->item(capturedRow, 0)->setText(typeStr);
                updateTotalsLabel(capturedPageIdx);

                for (int col = 0; col < table_->columnCount(); ++col) {
                    if (table_->item(capturedRow, col)) {
                        table_->item(capturedRow, col)->setBackground(QColor(180, 216, 228, 255));
                    }
                }
            }
        });

        if (pageModified) {
            for (int col = 0; col < table_->columnCount(); ++col) {
                if (table_->item(row, col)) {
                    table_->item(row, col)->setBackground(QColor(180, 216, 228, 255));
                }
            }
        }
    }

    // Populate ongoing row if present
    if (hasOngoing) {
        int row = static_cast<int>(comp.size());
        const auto& og = pendingTimelines_[idx].ongoing().value();
        QString typeStr = og.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(og.startTime.toString("hh:mm:ss") + " - " + og.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(og.duration) + "  (Running)");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(og.type == DurationType::Activity);
        box->setEnabled(false);
        table_->setCellWidget(row, 3, box);
    }

    // Update navigation button states
    prevButton_->setEnabled(pageIndex_ < pages_.size() - 1);
    nextButton_->setEnabled(pageIndex_ > 0);
}

void HistoryDialog::onOlder()
{
    if (pageIndex_ < pages_.size() - 1) {
        pageIndex_++;
        updateTable(pageIndex_);
    }
}

void HistoryDialog::onNewer()
{
    if (pageIndex_ > 0) {
        pageIndex_--;
        updateTable(pageIndex_);
    }
}

/**
 * Commits pending edits to the Application State and Database.
 *
 * Strategy:
 * 1. Applies in-memory changes to the page models.
 * 2. Aggregates historical + current-session rows and rewrites DB atomically.
 * 3. Updates TimeTracker runtime state only after persistence succeeds.
 *
 * Note: replaceDurationsInDB() rewrites the full durations table. Historical
 * rows are written as finalized, while current-session rows keep DB backing as
 * unfinalized so crash recovery can restore ongoing work.
 */
void HistoryDialog::saveChanges()
{
    if (result() != QDialog::Accepted) {
        Logger::Log("[HISTORY] Dialog cancelled, discarding changes");
        return;
    }

    // Collect durations from pendingTimelines_ and route by origin
    std::deque<TimeDuration> historyDurations;
    std::deque<TimeDuration> currentMemoryDurations;
    std::deque<TimeDuration> currentSessionDurations;
    std::optional<TimeDuration> ongoingDurationForSave;

    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].isCurrent) {
            const auto& comp = pendingTimelines_[i].completed();
            for (size_t row = 0; row < comp.size(); ++row) {
                if (isMemoryRow_[i][row]) {
                    currentMemoryDurations.push_back(comp[row]);
                    currentSessionDurations.push_back(comp[row]);
                } else {
                    historyDurations.push_back(comp[row]);
                }
            }
            ongoingDurationForSave = pendingTimelines_[i].ongoing();
        } else {
            const auto& comp = pendingTimelines_[i].completed();
            historyDurations.insert(historyDurations.end(), comp.begin(), comp.end());
        }
    }

    // Save historical + current-session durations to DB
    if (ongoingDurationForSave.has_value()) {
        currentSessionDurations.push_back(ongoingDurationForSave.value());
    }

    bool dbSaveSucceeded = true;
    if (!historyDurations.empty() || !currentSessionDurations.empty()) {
        Logger::Log(QString("[HISTORY] Saving %1 historical + %2 current-session durations to DB")
            .arg(historyDurations.size())
            .arg(currentSessionDurations.size()));

        dbSaveSucceeded = timetracker_.replaceAll(
            Timeline(historyDurations, std::nullopt),
            Timeline(currentSessionDurations, std::nullopt));
        if (!dbSaveSucceeded) {
            Logger::Log("[HISTORY] CRITICAL: Failed to save durations to DB");
            QMessageBox::critical(this, "Database Error",
                "Failed to save changes to the database. Your changes to historical entries may be lost.");
        } else {
            Logger::Log("[HISTORY] Successfully saved historical durations to DB");
        }
    } else {
        Logger::Log("[HISTORY] No historical durations to save");
    }

    // Keep runtime state unchanged when persistence fails. This prevents in-memory
    // edits from diverging from the database and avoids checkpoint tracking drift.
    if (!dbSaveSucceeded) {
        return;
    }

    // Atomically update TimeTracker's in-memory durations and checkpoint tracking.
    // replaceCurrentDurations couples both operations so callers cannot forget to
    // reset checkpoint tracking after replacing durations — the compiler enforces it.
    timetracker_.applyEdits(Timeline(currentMemoryDurations, ongoingDurationForSave));
    Logger::Log("[HISTORY] Updated TimeTracker current session and checkpoint tracking");
}

void HistoryDialog::showContextMenu(const QPoint& pos)
{
    int row = table_->rowAt(pos.y());
    if (row < 0) return;

    // Don't allow split on the ongoing row
    if (pageIndex_ < pendingTimelines_.size()
        && row == static_cast<int>(pendingTimelines_[pageIndex_].completed().size())
        && pendingTimelines_[pageIndex_].ongoing().has_value()) {
        return;
    }

    contextMenuRow_ = row;
    contextMenuPage_ = static_cast<int>(pageIndex_);
    QMenu menu(this);
    QAction* splitAction = menu.addAction("Split..");
    connect(splitAction, &QAction::triggered, this, &HistoryDialog::onSplitRow);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

/**
 * Handles the "Split" context menu action.
 *
 * Algorithm:
 * 1. Validates that the selected duration is long enough to be split (min 3s).
 * 2. Launches SplitDialog to get user input (split point and types).
 * 3. Validates the result to ensure no duration is lost or created (sum check).
 * 4. Atomically replaces the original entry with the two new entries in the deque.
 *
 * Note: Index-based modification is used to avoid iterator invalidation, as
 * the deque may be reallocated during insertion.
 */
void HistoryDialog::onSplitRow()
{
    if (contextMenuRow_ < 0) return;

    // Capture and reset row immediately to prevent stale state (Issue #5)
    int row = contextMenuRow_;
    contextMenuRow_ = -1;

    if (contextMenuPage_ < 0) {
        return;
    }
    uint idx = static_cast<uint>(contextMenuPage_);
    contextMenuPage_ = -1;

    if (idx >= pendingTimelines_.size() || row >= static_cast<int>(pendingTimelines_[idx].completed().size())) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: Invalid indices - page: %1, row: %2").arg(idx).arg(row));
        return;
    }
    // Don't allow split on the ongoing row
    if (row == static_cast<int>(pendingTimelines_[idx].completed().size())
        && pendingTimelines_[idx].ongoing().has_value()) {
        return;
    }

    const auto& completed = pendingTimelines_[idx].completed();
    const TimeDuration& duration = completed[row];
    qint64 originalDuration = duration.duration;
    QDateTime start = duration.startTime;
    QDateTime end = duration.endTime;

    // Check if duration is long enough to split meaningfully (at least 3 seconds)
    if (start.secsTo(end) < kMinimumSplitDurationSeconds) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: Duration too short to split - only %1 seconds")
            .arg(start.secsTo(end)));
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
            Logger::Log(QString("[HISTORY] Warning: Split duration mismatch - original: %1ms, split sum: %2ms, adjusting")
                .arg(originalDuration).arg(firstDuration + secondDuration));
            // Adjust second segment to preserve total duration
            secondDuration = originalDuration - firstDuration;
        }

        // Validate split results - ensure both segments have at least 1 second
        if (firstDuration < 1000 || secondDuration < 1000) {
            Logger::Log(QString("[HISTORY] Error: onSplitRow: Split results too small - first: %1ms, second: %2ms")
                .arg(firstDuration).arg(secondDuration));
            QMessageBox::warning(this, "Split Duration",
                "Invalid split: both segments must be at least 1 second long.");
            return;
        }

        DurationType firstType = dlg.getFirstSegmentType();
        DurationType secondType = dlg.getSecondSegmentType();

        // Perform the split via pure Timeline operation
        pendingTimelines_[idx] = pendingTimelines_[idx].withSplit(
            static_cast<size_t>(row), dlg.getSplitTime(), firstType, secondType);

        // Update isMemoryRow_: insert entry for second half with same value as first
        bool wasMemory = isMemoryRow_[idx][static_cast<size_t>(row)];
        isMemoryRow_[idx].insert(isMemoryRow_[idx].begin() + row + 1, wasMemory);

        assertPendingOriginsInvariant();

        updateTable(idx);
    }
}

/**
 * SplitDialog - Helper dialog for splitting a time duration.
 *
 * UX Design:
 * - Provides a slider to intuitively select the split point within the time range.
 * - Simplified Type Selection: Instead of allowing any combination of Activity/Pause,
 *   it restricts choice to "Activity->Pause" or "Pause->Activity".
 *   Why? Splitting "Activity->Activity" or "Pause->Pause" is semantically
 *   redundant and would just be merged back by cleanDurations().
 */

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
    if (totalSecs < kMinimumSplitDurationSeconds) {
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
    Logger::Log("[HISTORY] Dialog closing");

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

    pendingTimelines_.clear();
    isMemoryRow_.clear();
    assertPendingOriginsInvariant();

    // Resume checkpoints now that dialog is closed
    timetracker_.resumeCheckpoints();
}

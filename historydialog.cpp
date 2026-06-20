/**
 * HistoryDialog - Interface for viewing and editing past time logs.
 *
 * Architecture:
 * - Transactional Editing: All changes (type toggling, splitting) are performed
 *   on local copies (`pendingTimelines_`). The actual database and Timer
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
#include "timeformat.h"
#include "logger.h"
#include <QSlider>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QButtonGroup>
#include <algorithm>
#include <QHash>

namespace {

bool isSameSegmentId(const TimeDuration& a, const TimeDuration& b)
{
    if (a.segment_id.isEmpty() || b.segment_id.isEmpty()) {
        return false;
    }
    return a.segment_id == b.segment_id;
}
}

HistoryDialog::HistoryDialog(Timer& timetracker, const Settings& settings, QWidget* parent)
    : QDialog(parent), timetracker_(timetracker), settings_(settings), pageIndex_(0)
{
    // Pause checkpoints while dialog is open to prevent race conditions
    timetracker_.beginExclusiveEdit();

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

    // Group historical durations by date.
    // Same-day rows go into historyByDay (editable via Timeline/pendingTimelines_).
    // Cross-midnight rows go into crossMidnightByStartDay (display-only canonical)
    // and continuationsByEndDay (display-only spillover on end date).
    QMap<QDate, std::deque<TimeDuration>> historyByDay;
    QMap<QDate, std::deque<TimeDuration>> crossMidnightByStartDay;
    QMap<QDate, std::deque<TimeDuration>> continuationsByEndDay;
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
        if (isCrossMidnight(d.startTime, d.endTime)) {
            // Segments spanning more than one calendar boundary cannot occur in normal
            // Timer operation and have no safe display bucketing — skip them entirely.
            if (d.endTime.toLocalTime().date() > d.startTime.toLocalTime().date().addDays(1)) {
                qWarning("[HISTORY] createPages: skipping segment spanning >1 calendar boundary (segment_id: %s)",
                         qPrintable(d.segment_id.toString()));
                continue;
            }
            // Cross-midnight: show on start-day (canonical) and end-day (continuation).
            crossMidnightByStartDay[d.startTime.date()].push_back(d);
            continuationsByEndDay[d.endTime.date()].push_back(d);
            crossMidnightRows_.push_back(d);
            originIsMemory_.insert(d.segment_id.toString(), false);
        } else {
            historyByDay[d.startTime.date()].push_back(d);
        }
    }
    // Collect all dates that need a page.
    QSet<QDate> allDatesSet;
    for (const QDate& d : historyByDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : crossMidnightByStartDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : continuationsByEndDay.keys()) allDatesSet.insert(d);
    QList<QDate> historyDates = allDatesSet.values();
    std::sort(historyDates.begin(), historyDates.end(), std::greater<QDate>()); // Most recent first

    std::deque<TimeDuration> currentPageCompleted;
    currentPageCompleted.insert(currentPageCompleted.end(), currentDurations.begin(), currentDurations.end());
    for (const auto& d : currentDurations)
        originIsMemory_.insert(d.segment_id.toString(), true);

    auto todayIt = historyByDay.find(today);
    if (todayIt != historyByDay.end()) {
        currentPageCompleted.insert(currentPageCompleted.end(), todayIt->begin(), todayIt->end());
        for (const auto& d : *todayIt)
            originIsMemory_.insert(d.segment_id.toString(), false);
    }

    // Build currentPageDurations for the Page struct (includes ongoing for title count)
    std::deque<TimeDuration> currentPageDurations = currentPageCompleted;
    if (ongoingOpt.has_value()) {
        currentPageDurations.push_back(ongoingOpt.value());
    }

    // Add today page (current session + today's DB + ongoing)
    {
        std::deque<TimeDuration> todayCrossMidnight;
        auto cmIt = crossMidnightByStartDay.find(today);
        if (cmIt != crossMidnightByStartDay.end()) todayCrossMidnight = *cmIt;

        std::deque<TimeDuration> todayConts;
        auto contIt = continuationsByEndDay.find(today);
        if (contIt != continuationsByEndDay.end()) todayConts = *contIt;

        pages_.push_back({
            QString("Today (entries: ") + QString::number(currentPageDurations.size()) + QString(")"),
            currentPageDurations,
            todayCrossMidnight,
            todayConts,
            true
        });
    }

    // Add historical pages (one per day)
    for (const QDate& date : historyDates) {
        if (date == today) {
            continue;
        }
        std::deque<TimeDuration> crossMidnight;
        auto cmIt = crossMidnightByStartDay.find(date);
        if (cmIt != crossMidnightByStartDay.end()) crossMidnight = *cmIt;

        std::deque<TimeDuration> conts;
        auto contIt = continuationsByEndDay.find(date);
        if (contIt != continuationsByEndDay.end()) conts = *contIt;

        pages_.push_back({
            date.toString("yyyy-MM-dd") + QString(" (entries: ") + QString::number(historyByDay[date].size()) + QString(")"),
            historyByDay[date],
            crossMidnight,
            conts,
            false
        });
    }

    pendingTimelines_.reserve(pages_.size());
    pendingTimelines_.push_back(Timeline(currentPageCompleted, ongoingOpt));

    for (size_t i = 1; i < pages_.size(); ++i) {
        pendingTimelines_.push_back(Timeline(pages_[i].durations, std::nullopt));
        for (const auto& d : pages_[i].durations)
            originIsMemory_.insert(d.segment_id.toString(), false);
    }
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

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* okButton = new QPushButton("OK", this);
    QPushButton* cancelButton = new QPushButton("Cancel", this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

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
void HistoryDialog::addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix)
{
    const QColor grayBg(200, 200, 200, 180);
    QString typeStr = (d.type == DurationType::Activity ? "Activity  " : "Pause  ");
    typeStr += suffix;
    QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
    QTableWidgetItem* startEndItem = new QTableWidgetItem(
        d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
    QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
    for (auto* item : {typeItem, startEndItem, durationItem}) {
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        item->setBackground(grayBg);
    }
    table->setItem(row, 0, typeItem);
    table->setItem(row, 1, startEndItem);
    table->setItem(row, 2, durationItem);
}

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
    const auto& crossMidnight = pages_[idx].crossMidnight;
    const auto& conts = pages_[idx].continuations;
    int rowCount = static_cast<int>(comp.size());
    if (hasOngoing) ++rowCount;
    rowCount += static_cast<int>(crossMidnight.size());
    rowCount += static_cast<int>(conts.size());
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

        const SegmentId segId = d.segment_id;
        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, segId](int state) {
            if (capturedPageIdx != pageIndex_)
                return;
            const auto& comp = pendingTimelines_[capturedPageIdx].completed();
            auto it = std::find_if(comp.begin(), comp.end(),
                [&segId](const TimeDuration& td) { return td.segment_id == segId; });
            if (it == comp.end()) {
                Logger::Log(QString("[WARNING] Checkbox callback: segment_id not found"));
                return;
            }
            const size_t currentRow = static_cast<size_t>(std::distance(comp.begin(), it));
            DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            pendingTimelines_[capturedPageIdx] = pendingTimelines_[capturedPageIdx].withSegmentType(currentRow, newType);
            const int displayRow = static_cast<int>(currentRow);
            QString typeStr = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(typeStr);
                updateTotalsLabel(capturedPageIdx);
                for (int col = 0; col < table_->columnCount(); ++col) {
                    if (table_->item(displayRow, col))
                        table_->item(displayRow, col)->setBackground(QColor(180, 216, 228, 255));
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
        box->setEnabled(true);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, displayRow = row](int state) {
            if (capturedPageIdx != pageIndex_) return;
            if (!pendingTimelines_[capturedPageIdx].ongoing().has_value()) return;
            DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            auto ongoing = pendingTimelines_[capturedPageIdx].ongoing().value();
            ongoing.type = newType;
            pendingTimelines_[capturedPageIdx] = Timeline(pendingTimelines_[capturedPageIdx].completed(), ongoing);
            ongoingRowModified_ = true;
            const QString ts = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(ts);
                updateTotalsLabel(capturedPageIdx);
            }
        });
    }

    // Populate cross-midnight canonical rows (spans midnight, display-only on start-day page)
    {
        int baseRow = static_cast<int>(comp.size()) + (hasOngoing ? 1 : 0);
        for (int i = 0; i < static_cast<int>(crossMidnight.size()); ++i) {
            addReadOnlyRow(table_, baseRow + i, crossMidnight[i], "(spans midnight)");
        }
    }

    // Populate continuation rows (display-only cross-midnight spillover on end-day page)
    if (!conts.empty()) {
        int baseRow = static_cast<int>(comp.size()) + (hasOngoing ? 1 : 0)
                      + static_cast<int>(crossMidnight.size());
        for (int i = 0; i < static_cast<int>(conts.size()); ++i) {
            addReadOnlyRow(table_, baseRow + i, conts[i], "(cont.)");
        }
        // Column 3: no checkbox for continuation rows
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
 * 3. Updates Timer runtime state only after persistence succeeds.
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

    // Refresh ongoing end-time from engine unless the user edited the ongoing row.
    if (!ongoingRowModified_ && !pendingTimelines_.empty() && pages_[0].isCurrent) {
        const auto fresh = timetracker_.getOngoingDuration();
        const auto& cur  = pendingTimelines_[0].ongoing();
        const bool stillSameOngoing =
            fresh.has_value() && cur.has_value() &&
            fresh->segment_id == cur->segment_id;
        if (stillSameOngoing) {
            pendingTimelines_[0] = Timeline(
                pendingTimelines_[0].completed(), fresh);
        } else if (!fresh.has_value() && cur.has_value()) {
            // Engine has no valid ongoing (e.g. cross-midnight after a deferred
            // midnight stop). Clear it so commit() does not anchor a stale
            // start time in session_, which would orphan a checkpoint row.
            pendingTimelines_[0] = Timeline(
                pendingTimelines_[0].completed(), std::nullopt);
        }
    }

    // Step A: Build unified timeline (same-day rows only; cross-midnight handled separately)
    std::deque<TimeDuration> unifiedCompleted;
    std::optional<TimeDuration> ongoingDurationForSave;

    for (size_t i = 0; i < pages_.size(); ++i) {
        const auto& comp = pendingTimelines_[i].completed();
        for (const auto& d : comp)
            unifiedCompleted.push_back(d);
        if (pages_[i].isCurrent)
            ongoingDurationForSave = pendingTimelines_[i].ongoing();
    }

    // Capture memory-origin intervals before normalization so we can re-route merged rows
    // that absorbed current-session time back into the live session (fix for #6).
    struct OriginInterval { DurationType type; QDateTime start, end; };
    std::vector<OriginInterval> memoryIntervals;
    for (const auto& d : unifiedCompleted) {
        auto it = originIsMemory_.find(d.segment_id.toString());
        if (it != originIsMemory_.end() && it.value())
            memoryIntervals.push_back({d.type, d.startTime, d.endTime});
    }

    // Step B: Normalize once across all buckets; confirm if merges will occur (H4).
    // Cross-midnight rows are not included in normalization (Timeline rejects them);
    // they are preserved as-is and appended after normalization.
    const size_t preNormalizeSize = unifiedCompleted.size();
    Timeline normalised = Timeline(unifiedCompleted, std::nullopt).normalized();
    if (normalised.completed().size() < preNormalizeSize) {
        const auto answer = QMessageBox::question(this, "Overlapping Segments",
            "Overlapping segments were detected and will be merged. Continue?",
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    // Step C: Re-split using originIsMemory_
    std::deque<TimeDuration> historyDurations;
    std::deque<TimeDuration> currentMemoryDurations;
    std::deque<TimeDuration> currentSessionDurations;

    for (const auto& d : normalised.completed()) {
        auto idIt = originIsMemory_.find(d.segment_id.toString());
        Q_ASSERT(idIt != originIsMemory_.end());
        bool isMemory = (idIt != originIsMemory_.end()) && idIt.value();
        if (!isMemory) {
            // The surviving id is a history id, but the merged span may have absorbed a
            // current-session row. Keep it in the live session if it covers one.
            for (const auto& m : memoryIntervals) {
                if (m.type == d.type && m.start < d.endTime && d.startTime < m.end) {
                    isMemory = true;
                    break;
                }
            }
        }
        if (isMemory) {
            currentMemoryDurations.push_back(d);
            currentSessionDurations.push_back(d);
        } else {
            historyDurations.push_back(d);
        }
    }

    // Cross-midnight rows are preserved as-is in the history bucket.
    // They cannot go through Timeline normalization (same-day invariant), so they
    // are appended after normalization and saved via fromUnchecked().
    for (const auto& d : crossMidnightRows_)
        historyDurations.push_back(d);
    crossMidnightRows_.clear();

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
            Timeline::fromUnchecked(historyDurations),
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

    // Atomically update Timer's in-memory durations and checkpoint tracking.
    // replaceCurrentDurations couples both operations so callers cannot forget to
    // reset checkpoint tracking after replacing durations — the compiler enforces it.
    timetracker_.commit(Timeline(currentMemoryDurations, ongoingDurationForSave));
    Logger::Log("[HISTORY] Updated Timer current session and checkpoint tracking");
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

    // Don't allow split on continuation rows (display-only)
    if (pageIndex_ < pages_.size()) {
        const int canonicalRows = static_cast<int>(pendingTimelines_[pageIndex_].completed().size())
                                  + (pendingTimelines_[pageIndex_].ongoing().has_value() ? 1 : 0);
        if (row >= canonicalRows) {
            return;
        }
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

    // Show split dialog; preset type based on source row (Activity→Pause or Pause→Activity).
    SplitDialog dlg(start, end, this);
    dlg.setFirstSegmentType(duration.type);
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

        // Capture the original segment_id before the split.
        const QString origId = completed[row].segment_id.toString();
        const bool wasMemory = originIsMemory_.value(origId, true);
        const size_t prevSize = completed.size();

        pendingTimelines_[idx] = pendingTimelines_[idx].withSplit(
            static_cast<size_t>(row), dlg.getSplitTime(), firstType, secondType);

        // Register the second-half's new segment_id with the same origin as the first.
        // Only if the split actually happened (size grew by 1).
        const auto& newComp = pendingTimelines_[idx].completed();
        if (newComp.size() > prevSize && row + 1 < static_cast<int>(newComp.size()))
            originIsMemory_.insert(newComp[row + 1].segment_id.toString(), wasMemory);

        updateTable(idx);
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

    // Resume checkpoints now that dialog is closed
    timetracker_.endExclusiveEdit();
}

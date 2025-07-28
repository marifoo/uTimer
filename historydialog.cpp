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
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &HistoryDialog::showContextMenu);

    // Log initial state of containers
    if (settings_.logToFile()) {
        Logger::Log("  [HISTORY] Dialog opened");
        
        // Get total rows in database
        auto historyDurations = timetracker_.getDurationsHistory();
        Logger::Log(QString("  [HISTORY] Loaded %1 total durations from DB").arg(historyDurations.size()));

        for (size_t i = 0; i < pages_.size(); ++i) {
            Logger::Log(QString("  [HISTORY] Page %1 - Title: %2, Entries: %3, IsCurrent: %4")
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

    QMap<QDate, std::vector<TimeDuration>> historyByDay;
    for (const auto& d : historyDurations) {
        historyByDay[d.endTime.date()].push_back(d);
    }
    QList<QDate> historyDates = historyByDay.keys();
    std::sort(historyDates.begin(), historyDates.end(), std::greater<QDate>());

    pages_.push_back({
        QString("Current Session (entries: ") + QString::number(currentDurations.size()) + QString(")"),
        std::vector<TimeDuration>(currentDurations.begin(), currentDurations.end()),
        true
    });

    for (const QDate& date : historyDates) {
        pages_.push_back({
            date.toString("yyyy-MM-dd") + QString(" (entries: ") + QString::number(historyByDay[date].size()) + QString(")"),
            historyByDay[date],
            false
        });
    }

    edits_.resize(pages_.size());
    for (size_t i = 0; i < pages_.size(); ++i) {
        edits_[i].resize(pages_[i].durations.size());
        for (size_t j = 0; j < pages_[i].durations.size(); ++j) {
            edits_[i][j] = pages_[i].durations[j].type;
        }
    }
    editedRows_.resize(pages_.size());
}

void HistoryDialog::setupUI()
{
    setWindowTitle("History");
    QVBoxLayout* layout = new QVBoxLayout(this);

    pageLabel_ = new QLabel(this);
    layout->addWidget(pageLabel_);

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

    connect(prevButton_, &QPushButton::clicked, this, &HistoryDialog::onPrevClicked);
    connect(nextButton_, &QPushButton::clicked, this, &HistoryDialog::onNextClicked);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    resize(400, 400);
}

std::pair<qint64, qint64> HistoryDialog::calculateTotals(const std::vector<DurationType>& types, const std::vector<TimeDuration>& durations)
{
    qint64 totalActivity = 0;
    qint64 totalPause = 0;
    for (size_t i = 0; i < durations.size(); ++i) {
        if (types[i] == DurationType::Activity)
            totalActivity += durations[i].duration;
        else
            totalPause += durations[i].duration;
    }
    return std::make_pair(totalActivity, totalPause);
}

void HistoryDialog::updateTotalsLabel(uint idx)
{
    auto totals = calculateTotals(edits_[idx], pages_[idx].durations);
    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totals.first) + QString("  Pause: ") + convMSecToTimeStr(totals.second);
    pageLabel_->setText(pages_[idx].title + totalsStr);
}

void HistoryDialog::updateTable(uint idx)
{
    table_->clearContents();
    table_->setRowCount(static_cast<int>(pages_[idx].durations.size()));
    updateTotalsLabel(idx);

    for (int row = 0; row < int(pages_[idx].durations.size()); ++row) {
        const auto& d = pages_[idx].durations[row];
        QString typeStr = (edits_[idx][row] == DurationType::Activity) ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(d.endTime.addMSecs(-d.duration).toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(edits_[idx][row] == DurationType::Activity);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, [this, idx, row](int state) {
            edits_[idx][row] = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            QString typeStr = (edits_[idx][row] == DurationType::Activity) ? "Activity  " : "Pause  ";
            table_->item(row, 0)->setText(typeStr);
            updateTotalsLabel(idx);
            editedRows_[idx].insert(row);
            for (int col = 0; col < table_->columnCount(); ++col) {
                if (table_->item(row, col))
                    table_->item(row, col)->setBackground(QColor(180, 216, 228, 255));
            }
        });

        // Highlight edited/split rows
        if (editedRows_.size() > idx && editedRows_[idx].count(row)) {
            for (int col = 0; col < table_->columnCount(); ++col) {
                if (table_->item(row, col))
                    table_->item(row, col)->setBackground(QColor(180, 216, 228, 255));
            }
        }
    }

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
    if (result() == QDialog::Accepted) {
        // Check if there are no changes to save
        bool hasChanges = false;
        for (const auto& editedRows : editedRows_) {
            if (!editedRows.empty()) {
                hasChanges = true;
                break;
            }
        }

        if (!hasChanges) {
            if (settings_.logToFile()) {
                Logger::Log("  [HISTORY] No changes to save, returning early");
            }
            return;
        }

        if (settings_.logToFile())
            Logger::Log("  [HISTORY] Dialog accepted, saving changes");
        
        // Log state before saving
        if (settings_.logToFile()) {
            for (size_t i = 0; i < pages_.size(); ++i) {
                Logger::Log(QString("  [HISTORY] Page %1 - Entries: %2, EditedRows: %3")
                    .arg(i)
                    .arg(pages_[i].durations.size())
                    .arg(editedRows_[i].size()));
            }
        }

        // Update types for all pages
        for (size_t i = 0; i < pages_.size(); ++i) {
            for (size_t j = 0; j < pages_[i].durations.size(); ++j) {
                if (pages_[i].isCurrent) {
                    timetracker_.setDurationType(j, edits_[i][j]);
                } else {
                    pages_[i].durations[j].type = edits_[i][j];
                }
            }
        }
        // Save all durations (current session + history) to DB
        std::deque<TimeDuration> allDurations;
        for (size_t i = 0; i < pages_.size(); ++i) {
            allDurations.insert(allDurations.end(), pages_[i].durations.begin(), pages_[i].durations.end());
        }
        if (!allDurations.empty()) {
            if (settings_.logToFile())
                Logger::Log(QString("  [HISTORY] Saving %1 total durations to DB").arg(allDurations.size()));
            bool result = timetracker_.replaceDurationsInDB(allDurations);
            if (!result) {
                if (settings_.logToFile())
                    Logger::Log("  [HISTORY] Failed to save durations to DB");
            } else {
                if (settings_.logToFile())
                    Logger::Log("  [HISTORY] Successfully saved durations to DB");
            }
        }
    } else {
        if (settings_.logToFile()) {
            Logger::Log("  [HISTORY] Dialog cancelled, discarding changes");
            // Log final state even when cancelling
            for (size_t i = 0; i < pages_.size(); ++i) {
                Logger::Log(QString("  [HISTORY] Page %1 - Title: %2, Entries: %3")
                    .arg(i)
                    .arg(pages_[i].title)
                    .arg(pages_[i].durations.size()));
            }
        }
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
    uint idx = pageIndex_;

    // Log initial state
    if (settings_.logToFile()) {
        Logger::Log(QString("    [SPLIT] Starting split operation. Page index: %1, Row: %2").arg(idx).arg(contextMenuRow_));
        Logger::Log(QString("    [SPLIT] Container sizes - Pages: %1, Current page durations: %2, Edits: %3")
            .arg(pages_.size())
            .arg(pages_[idx].durations.size())
            .arg(edits_[idx].size()));
        Logger::Log(QString("    [SPLIT] Is current session: %1").arg(pages_[idx].isCurrent));
    }

    TimeDuration& duration = pages_[idx].durations[contextMenuRow_];
    QDateTime start = duration.endTime.addMSecs(-duration.duration);
    QDateTime end = duration.endTime;
    
    if (settings_.logToFile()) {
        Logger::Log(QString("    [SPLIT] Original duration - Start: %1, End: %2, Type: %3")
            .arg(start.toString("hh:mm:ss"))
            .arg(end.toString("hh:mm:ss"))
            .arg(duration.type == DurationType::Activity ? "Activity" : "Pause"));
    }

    SplitDialog dlg(start, end, this);
    if (dlg.exec() == QDialog::Accepted) {
        QDateTime splitTime = dlg.getSplitTime();
        qint64 firstDuration = start.msecsTo(splitTime);
        qint64 secondDuration = splitTime.msecsTo(end);
        DurationType firstType = dlg.getFirstSegmentType();
        DurationType secondType = dlg.getSecondSegmentType();

        if (settings_.logToFile()) {
            Logger::Log(QString("    [SPLIT] Split point: %1").arg(splitTime.toString("hh:mm:ss")));
            Logger::Log(QString("    [SPLIT] Durations - First: %1, Second: %2")
                .arg(convMSecToTimeStr(firstDuration))
                .arg(convMSecToTimeStr(secondDuration)));
            Logger::Log(QString("    [SPLIT] Types - First: %1, Second: %2")
                .arg(firstType == DurationType::Activity ? "Activity" : "Pause")
                .arg(secondType == DurationType::Activity ? "Activity" : "Pause"));
        }

        TimeDuration first(firstType, firstDuration, splitTime);
        TimeDuration second(secondType, secondDuration, end);

        auto& durationsVec = pages_[idx].durations;
        durationsVec.erase(durationsVec.begin() + contextMenuRow_);
        durationsVec.insert(durationsVec.begin() + contextMenuRow_, second);
        durationsVec.insert(durationsVec.begin() + contextMenuRow_, first);

        auto& editsVec = edits_[idx];
        editsVec.erase(editsVec.begin() + contextMenuRow_);
        editsVec.insert(editsVec.begin() + contextMenuRow_, secondType);
        editsVec.insert(editsVec.begin() + contextMenuRow_, firstType);

        // Mark both split rows as edited
        if (editedRows_.size() > idx) {
            editedRows_[idx].insert(contextMenuRow_);
            editedRows_[idx].insert(contextMenuRow_ + 1);
        }

        // Log final state after split
        if (settings_.logToFile()) {
            Logger::Log(QString("    [SPLIT] After split - Page durations: %1, Edits: %2")
                .arg(pages_[idx].durations.size())
                .arg(edits_[idx].size()));
        }

        // If current session, also update TimeTracker's durations_ using setCurrentDurations
        if (pages_[idx].isCurrent) {
            timetracker_.setCurrentDurations(pages_[idx].durations);
            if (settings_.logToFile())
                Logger::Log("    [SPLIT] Updated TimeTracker current durations");
        }
        updateTable(idx);
    }
}

SplitDialog::SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent)
    : QDialog(parent), start_(start), end_(end), firstSegmentType_(DurationType::Activity), secondSegmentType_(DurationType::Activity)
{
    setWindowTitle("Split Duration");
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* rowLayout = new QHBoxLayout();
    QLabel* startLabel = new QLabel(QString("Start: %1").arg(start.toString("hh:mm:ss")), this);
    QLabel* endLabel = new QLabel(QString("End: %1").arg(end.toString("hh:mm:ss")), this);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setMinimum(1);
    int totalSecs = start.secsTo(end) - 1;
    slider_->setMaximum(totalSecs);
    slider_->setValue(totalSecs / 2);

    QFont timeFont = startLabel->font();
    timeFont.setPointSize(timeFont.pointSize() + 2); // Increase font size
    startLabel->setFont(timeFont);
    endLabel->setFont(timeFont);

    rowLayout->addWidget(startLabel);
    rowLayout->addWidget(slider_);
    rowLayout->addWidget(endLabel);
    layout->addLayout(rowLayout);

    splitTimeLabel_ = new QLabel(this);
    splitTimeLabel_->setAlignment(Qt::AlignCenter); // Center the label
    splitTimeLabel_->setFont(timeFont); // Apply increased font size
    layout->addWidget(splitTimeLabel_);
    updateSplitLabel(slider_->value());
    connect(slider_, &QSlider::valueChanged, this, &SplitDialog::updateSplitLabel);

    // Add four-column layout for segment types
    QHBoxLayout* segmentTypeLayout = new QHBoxLayout();

    // Left Flexible Empty Column
    QSpacerItem* leftSpacer = new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Minimum);
    segmentTypeLayout->addItem(leftSpacer);

    // First Segment Column
    QVBoxLayout* firstSegmentLayout = new QVBoxLayout();
    QLabel* firstSegmentLabel = new QLabel("First Segment:", this);
    firstSegmentActivity_ = new QRadioButton("Activity", this);
    firstSegmentPause_ = new QRadioButton("Pause", this);
    firstSegmentActivity_->setChecked(true);

    QButtonGroup* firstSegmentGroup = new QButtonGroup(this);
    firstSegmentGroup->addButton(firstSegmentActivity_);
    firstSegmentGroup->addButton(firstSegmentPause_);

    firstSegmentLayout->addWidget(firstSegmentLabel);
    firstSegmentLayout->addWidget(firstSegmentActivity_);
    firstSegmentLayout->addWidget(firstSegmentPause_);
    segmentTypeLayout->addLayout(firstSegmentLayout);

    // Second Segment Column
    QVBoxLayout* secondSegmentLayout = new QVBoxLayout();
    QLabel* secondSegmentLabel = new QLabel("Second Segment:", this);
    secondSegmentActivity_ = new QRadioButton("Activity", this);
    secondSegmentPause_ = new QRadioButton("Pause", this);
    secondSegmentActivity_->setChecked(true);

    QButtonGroup* secondSegmentGroup = new QButtonGroup(this);
    secondSegmentGroup->addButton(secondSegmentActivity_);
    secondSegmentGroup->addButton(secondSegmentPause_);

    secondSegmentLayout->addWidget(secondSegmentLabel);
    secondSegmentLayout->addWidget(secondSegmentActivity_);
    secondSegmentLayout->addWidget(secondSegmentPause_);
    segmentTypeLayout->addLayout(secondSegmentLayout);

    // Right Flexible Empty Column
    QSpacerItem* rightSpacer = new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Minimum);
    segmentTypeLayout->addItem(rightSpacer);

    layout->addLayout(segmentTypeLayout);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(450, height());
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
    return firstSegmentActivity_->isChecked() ? DurationType::Activity : DurationType::Pause;
}

DurationType SplitDialog::getSecondSegmentType() const
{
    return secondSegmentActivity_->isChecked() ? DurationType::Activity : DurationType::Pause;
}

void SplitDialog::setFirstSegmentType(DurationType type)
{
    if (type == DurationType::Activity) {
        firstSegmentActivity_->setChecked(true);
    } else {
        firstSegmentPause_->setChecked(true);
    }
}

void SplitDialog::setSecondSegmentType(DurationType type)
{
    if (type == DurationType::Activity) {
        secondSegmentActivity_->setChecked(true);
    } else {
        secondSegmentPause_->setChecked(true);
    }
}

HistoryDialog::~HistoryDialog() {
    if (settings_.logToFile()) {
        Logger::Log("  [HISTORY] Dialog closing");
    }
    editedRows_.clear();
}
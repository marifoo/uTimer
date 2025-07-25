#include "historydialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QPushButton>
#include "helpers.h"

HistoryDialog::HistoryDialog(TimeTracker& timetracker, QWidget* parent)
    : QDialog(parent), timetracker_(timetracker), pageIndex_(0)
{
    createPages();
    setupUI();
    updateTable(pageIndex_);
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

    resize(500, 400);
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

void HistoryDialog::updateTotalsLabel(int idx)
{
    auto totals = calculateTotals(edits_[idx], pages_[idx].durations);
    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totals.first) + QString("  Pause: ") + convMSecToTimeStr(totals.second);
    pageLabel_->setText(pages_[idx].title + totalsStr);
}

void HistoryDialog::updateTable(int idx)
{
    table_->clearContents();
    table_->setRowCount(static_cast<int>(pages_[idx].durations.size()));
    updateTotalsLabel(idx);

    for (int row = 0; row < int(pages_[idx].durations.size()); ++row) {
        const auto& d = pages_[idx].durations[row];
        QString typeStr = (edits_[idx][row] == DurationType::Activity) ? "Activity    " : "Pause    ";
        table_->setItem(row, 0, new QTableWidgetItem(typeStr));

        QString startEndStr = d.endTime.addMSecs(-d.duration).toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss");
        table_->setItem(row, 1, new QTableWidgetItem(startEndStr));

        QString durationStr = convMSecToTimeStr(d.duration) + "  ";
        table_->setItem(row, 2, new QTableWidgetItem(durationStr));

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(edits_[idx][row] == DurationType::Activity);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, [this, idx, row](int state) {
            edits_[idx][row] = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            QString typeStr = (edits_[idx][row] == DurationType::Activity) ? "Activity *  " : "Pause *  ";
            table_->item(row, 0)->setText(typeStr);
            updateTotalsLabel(idx);
        });
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
        for (size_t i = 0; i < pages_.size(); ++i) {
            for (size_t j = 0; j < pages_[i].durations.size(); ++j) {
                if (pages_[i].isCurrent) {
                    timetracker_.setDurationType(j, edits_[i][j]);
                } else {
                    pages_[i].durations[j].type = edits_[i][j];
                }
            }
        }
        std::deque<TimeDuration> allHistory;
        for (size_t i = 1; i < pages_.size(); ++i) {
            allHistory.insert(allHistory.end(), pages_[i].durations.begin(), pages_[i].durations.end());
        }
        if (!allHistory.empty()) {
            timetracker_.replaceDurationsInDB(allHistory);
        }
    }
}
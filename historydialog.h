#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include <QMap>
#include <vector>
#include <deque>
#include <set>
#include "types.h"
#include "timetracker.h"

class QLabel;
class QTableWidget;
class QPushButton;
class QSlider;
class QRadioButton;

class SplitDialog;

class HistoryDialog : public QDialog
{
    Q_OBJECT

private:
    enum class RowOrigin {CurrentMemory, CurrentDatabase, HistoricalDatabase, Ongoing};
    struct Page {
        QString title;
        std::deque<TimeDuration> durations;
        bool isCurrent;
    };

    TimeTracker& timetracker_;
    const Settings& settings_;
    std::vector<Page> pages_;
    std::vector<std::deque<TimeDuration>> pendingChanges_; // Store the entire edited page for each page index
    std::vector<std::vector<RowOrigin>> rowOrigins_;
    uint pageIndex_;
    int contextMenuRow_ = -1;
    int contextMenuPage_ = -1;

    QLabel* pageLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void createPages();
    void updateTotalsLabel(uint idx);
    void updateTable(uint idx);
    std::pair<qint64, qint64> calculateTotals(const std::deque<TimeDuration>& durations);
    void saveChanges();
    void showContextMenu(const QPoint& pos);

private slots:
    void onPrevClicked();
    void onNextClicked();
    void onSplitRow();

public:
    explicit HistoryDialog(TimeTracker& timetracker, const Settings& settings, QWidget* parent = nullptr);
    ~HistoryDialog();

    int exec() override {
        int result = QDialog::exec();
        if (result == QDialog::Accepted) {
            saveChanges();
        }
        return result;
    }
};

class SplitDialog : public QDialog
{
    Q_OBJECT

public:
    SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent = nullptr);

    QDateTime getSplitTime() const;
    DurationType getFirstSegmentType() const;
    DurationType getSecondSegmentType() const;

    void setFirstSegmentType(DurationType type);
    void setSecondSegmentType(DurationType type);

private slots:
    void updateSplitLabel(int value);

private:
    QDateTime start_;
    QDateTime end_;
    QSlider* slider_;
    QLabel* splitTimeLabel_;

    QRadioButton* activityPauseOption_;
    QRadioButton* pauseActivityOption_;
};

#endif // HISTORYDIALOG_H

#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include <QMap>
#include <QHash>
#include <optional>
#include <vector>
#include <deque>
#include <set>
#include "types.h"
#include "timer.h"
#include "timeline.h"

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
    struct Page {
        QString title;
        std::deque<TimeDuration> durations;
        bool isCurrent;
    };

    Timer& timetracker_;
    const Settings& settings_;
    std::vector<Page> pages_;
    std::vector<Timeline> pendingTimelines_;
    QHash<QString, bool> originIsMemory_;  // segment_id.toString() → true if memory row
    uint pageIndex_;
    int contextMenuRow_ = -1;
    int contextMenuPage_ = -1;

    QLabel* pageLabel_;
    QLabel* loadReconciliationLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void createPages();
    void updateTotalsLabel(uint idx);
    void updateTable(uint idx);
    void saveChanges();
    void showContextMenu(const QPoint& pos);
    QString buildLoadReconciliationMessage() const;

private slots:
    void onOlder();
    void onNewer();
    void onSplitRow();

public:
    explicit HistoryDialog(Timer& timetracker, const Settings& settings, QWidget* parent = nullptr);
    ~HistoryDialog();
    QString getLoadReconciliationMessage() const;

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

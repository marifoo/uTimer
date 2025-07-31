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
    struct Page {
        QString title;
        std::vector<TimeDuration> durations;
        bool isCurrent;
    };

    TimeTracker& timetracker_;
    const Settings& settings_;
    std::vector<Page> pages_;
    std::vector<std::vector<TimeDuration>> pendingChanges_; // Store the entire edited page for each page index
    uint pageIndex_;
    int contextMenuRow_ = -1;

    QLabel* pageLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void createPages();
    void updateTotalsLabel(uint idx);
    void updateTable(uint idx);
    std::pair<qint64, qint64> calculateTotals(const std::vector<TimeDuration>& durations);
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

    QRadioButton* firstSegmentActivity_;
    QRadioButton* firstSegmentPause_;
    QRadioButton* secondSegmentActivity_;
    QRadioButton* secondSegmentPause_;

    DurationType firstSegmentType_;
    DurationType secondSegmentType_;
};

#endif // HISTORYDIALOG_H#endif // HISTORYDIALOG_H
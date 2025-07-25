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
    std::vector<Page> pages_;
    std::vector<std::vector<DurationType>> edits_;
    std::vector<std::set<int>> editedRows_; // Track edited/split rows per page
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
    std::pair<qint64, qint64> calculateTotals(const std::vector<DurationType>& types, const std::vector<TimeDuration>& durations);
    void saveChanges();
    void showContextMenu(const QPoint& pos);

private slots:
    void onPrevClicked();
    void onNextClicked();
    void onSplitRow();

public:
    explicit HistoryDialog(TimeTracker& timetracker, QWidget* parent = nullptr);
    ~HistoryDialog();

    int exec() override {
        int result = QDialog::exec();
        if (result == QDialog::Accepted) {
            saveChanges();
        }
        return result;
    }
};

class SplitDialog : public QDialog {
    Q_OBJECT
public:
    SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent = nullptr);
    QDateTime getSplitTime() const;
private:
    QSlider* slider_;
    QLabel* splitTimeLabel_;
    QDateTime start_;
    QDateTime end_;
    void updateSplitLabel(int value);
};

#endif // HISTORYDIALOG_H
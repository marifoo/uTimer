#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include <QMap>
#include <vector>
#include <deque>
#include "types.h"
#include "timetracker.h"

class QLabel;
class QTableWidget;
class QPushButton;

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
    uint pageIndex_;

    QLabel* pageLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void createPages();
    void updateTotalsLabel(int idx);
    void updateTable(int idx);
    std::pair<qint64, qint64> calculateTotals(const std::vector<DurationType>& types, const std::vector<TimeDuration>& durations);
    void saveChanges();

private slots:
    void onPrevClicked();
    void onNextClicked();

public:
    explicit HistoryDialog(TimeTracker& timetracker, QWidget* parent = nullptr);
    ~HistoryDialog() = default;

    int exec() override {
        int result = QDialog::exec();
        if (result == QDialog::Accepted) {
            saveChanges();
        }
        return result;
    }
};

#endif // HISTORYDIALOG_H
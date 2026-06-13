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
#include "splitdialog.h"

class QLabel;
class QTableWidget;
class QPushButton;

class HistoryDialog : public QDialog
{
    Q_OBJECT

private:
    struct Page {
        QString title;
        std::deque<TimeDuration> durations;          // same-day canonical (editable) rows
        std::deque<TimeDuration> crossMidnight;      // cross-midnight rows starting on this day (display-only)
        std::deque<TimeDuration> continuations;      // cross-midnight rows ending on this day (display-only)
        bool isCurrent;
    };

    Timer& timetracker_;
    const Settings& settings_;
    std::vector<Page> pages_;
    std::vector<Timeline> pendingTimelines_;
    QHash<QString, bool> originIsMemory_;  // segment_id.toString() → true if memory row
    std::deque<TimeDuration> crossMidnightRows_;  // cross-midnight rows preserved through save
    bool ongoingRowModified_ = false;
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
    void addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix);
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

#endif // HISTORYDIALOG_H

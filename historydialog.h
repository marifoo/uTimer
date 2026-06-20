#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include <optional>
#include "types.h"
#include "timer.h"
#include "timeline.h"
#include "splitdialog.h"
#include "historyeditsession.h"

class QLabel;
class QTableWidget;
class QPushButton;

/**
 * TimerExclusiveEditGuard — RAII guard for Timer::beginExclusiveEdit / endExclusiveEdit.
 *
 * Calls beginExclusiveEdit() on construction and endExclusiveEdit() on destruction,
 * so resume cannot be forgotten on accept, reject, or early-return paths.
 */
class TimerExclusiveEditGuard {
public:
    explicit TimerExclusiveEditGuard(Timer& timer) : timer_(timer) {
        timer_.beginExclusiveEdit();
    }
    ~TimerExclusiveEditGuard() {
        timer_.endExclusiveEdit();
    }

    TimerExclusiveEditGuard(const TimerExclusiveEditGuard&) = delete;
    TimerExclusiveEditGuard& operator=(const TimerExclusiveEditGuard&) = delete;

private:
    Timer& timer_;
};

class HistoryDialog : public QDialog
{
    Q_OBJECT

private:
    Timer& timetracker_;
    const Settings& settings_;

    /// RAII guard: beginExclusiveEdit on construction, endExclusiveEdit on destruction.
    TimerExclusiveEditGuard exclusiveEditGuard_;

    /// Non-widget model owning all edit state: pages, pending timelines, origin tracking.
    HistoryEditSession session_;

    uint pageIndex_;
    int contextMenuRow_ = -1;
    int contextMenuPage_ = -1;

    QLabel* pageLabel_;
    QLabel* loadReconciliationLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void updateTotalsLabel(uint idx);
    void updateTable(uint idx);
    void addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix);
    void showContextMenu(const QPoint& pos);
    QString buildLoadReconciliationMessage() const;

private slots:
    void onOlder();
    void onNewer();
    void onSplitRow();

public:
    explicit HistoryDialog(Timer& timetracker, const Settings& settings, QWidget* parent = nullptr);
    ~HistoryDialog() override;
    QString getLoadReconciliationMessage() const;

    /**
     * Commits pending edits to the database and Timer. Returns true on success.
     * Returns false (and keeps the dialog open / exits early) if:
     *   - The dialog result is not Accepted (cancelled path).
     *   - A merge confirmation was declined by the user.
     *   - The DB save failed (shows a critical error dialog).
     *
     * Called by accept() to block close on failure. Can also be called directly
     * in tests (via #define private public) to exercise the commit path explicitly.
     */
    bool saveChanges();

    /// Overrides QDialog::accept() to run saveChanges() before closing.
    /// If saveChanges() returns false the dialog stays open.
    void accept() override;
};

#endif // HISTORYDIALOG_H

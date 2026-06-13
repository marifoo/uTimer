#ifndef SPLITDIALOG_H
#define SPLITDIALOG_H

#include <QDialog>
#include <QDateTime>
#include "types.h"

class QLabel;
class QSlider;
class QRadioButton;

/// Minimum segment duration (in seconds) that can be split meaningfully.
constexpr int kMinimumSplitDurationSeconds = 3;

/**
 * SplitDialog — modal dialog for splitting a single TimeDuration into two.
 *
 * Takes the start and end time of an existing segment. The user drags a slider
 * to choose a split point, and picks the type assignment for each half
 * (Activity→Pause or Pause→Activity). No dependency on HistoryDialog internals.
 */
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

#endif // SPLITDIALOG_H

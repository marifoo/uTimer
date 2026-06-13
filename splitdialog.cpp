#include "splitdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QRadioButton>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <algorithm>

SplitDialog::SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent)
    : QDialog(parent), start_(start), end_(end)
{
    setWindowTitle("Split Duration");
    QVBoxLayout* layout = new QVBoxLayout(this);

    // Time range display with slider
    QHBoxLayout* rowLayout = new QHBoxLayout();
    QLabel* startLabel = new QLabel(QString("Start: %1").arg(start.toString("hh:mm:ss")), this);
    QLabel* endLabel = new QLabel(QString("End: %1").arg(end.toString("hh:mm:ss")), this);
    slider_ = new QSlider(Qt::Horizontal, this);

    int totalSecs = start.secsTo(end);
    if (totalSecs < kMinimumSplitDurationSeconds) {
        // Duration too short to split meaningfully
        slider_->setMinimum(1);
        slider_->setMaximum(1);
        slider_->setValue(1);
        slider_->setEnabled(false);
    } else {
        slider_->setMinimum(1);
        slider_->setMaximum(totalSecs - 1);
        slider_->setValue(totalSecs / 2); // Start at midpoint
    }

    rowLayout->addWidget(startLabel);
    rowLayout->addWidget(slider_);
    rowLayout->addWidget(endLabel);
    layout->addLayout(rowLayout);

    // Split time display
    splitTimeLabel_ = new QLabel(this);
    splitTimeLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(splitTimeLabel_);
    updateSplitLabel(slider_->value());
    connect(slider_, &QSlider::valueChanged, this, &SplitDialog::updateSplitLabel);

    // Segment type selection - simplified to 2 radio buttons
    QVBoxLayout* segmentTypeLayout = new QVBoxLayout();
    QLabel* segmentTypeLabel = new QLabel("Split Type:", this);
    layout->addWidget(segmentTypeLabel);

    activityPauseOption_ = new QRadioButton("First: Activity, Second: Pause", this);
    pauseActivityOption_ = new QRadioButton("First: Pause, Second: Activity", this);

    // Default to "Pause -> Activity" as requested
    pauseActivityOption_->setChecked(true);

    QButtonGroup* segmentGroup = new QButtonGroup(this);
    segmentGroup->addButton(activityPauseOption_);
    segmentGroup->addButton(pauseActivityOption_);

    segmentTypeLayout->addWidget(activityPauseOption_);
    segmentTypeLayout->addWidget(pauseActivityOption_);
    layout->addLayout(segmentTypeLayout);

    // Dialog buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Ensure minimum dialog size for very short durations
    resize(450, std::max(height(), 120));
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
    return activityPauseOption_->isChecked() ? DurationType::Activity : DurationType::Pause;
}

DurationType SplitDialog::getSecondSegmentType() const
{
    return activityPauseOption_->isChecked() ? DurationType::Pause : DurationType::Activity;
}

void SplitDialog::setFirstSegmentType(DurationType type)
{
    if (type == DurationType::Activity) {
        activityPauseOption_->setChecked(true);
    } else {
        pauseActivityOption_->setChecked(true);
    }
}

void SplitDialog::setSecondSegmentType(DurationType type)
{
    // Since we have only 2 mutually exclusive options,
    // setting the second segment type determines the first as well
    if (type == DurationType::Pause) {
        activityPauseOption_->setChecked(true);  // Activity -> Pause
    } else {
        pauseActivityOption_->setChecked(true);  // Pause -> Activity
    }
}

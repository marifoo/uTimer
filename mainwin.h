#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QString>
#include "contentwidget.h"
#include "settings.h"
#include "types.h"

class MainWin : public QMainWindow
{
	Q_OBJECT
private:
	ContentWidget *content_widget_;
	QSystemTrayIcon *tray_icon_;
	const Settings & settings_;

	bool warning_activity_shown_;
	bool warning_pause_shown_;

	void updateTrayIconTooltip(QString activity, QString pause);
	void showMsgBox(const QString &text);
	void showMainWin();
	void toggleAlwaysOnTopFlag();
	void showActivityWarnings(const qint64 &t_active, const qint64 &t_pause);
	void setupIcon();
	void setupCentralWidget(Settings &settings);

public:
	explicit MainWin(Settings & settings, QWidget *parent = nullptr);
	void start();

signals:
	void sendButtons(Button button);

public slots:
	void updateAllTimes(qint64 t_active, qint64 t_pause);	
	void iconActivated(QSystemTrayIcon::ActivationReason reason);
	void minToTray();
	void toggleAlwaysOnTop();
	void reactOnLockState(LockEvent event);
};

#endif // MAINWIN_H

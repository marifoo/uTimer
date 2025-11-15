#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QString>
#include <QTimer>
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
	TimeTracker& timetracker_;

	bool warning_activity_shown_;
	bool warning_pause_shown_;
	bool was_active_before_autopause_;
	
	// Midnight auto-stop/restart handling
	QTimer* midnight_timer_;
	
	void scheduleMidnightStop();
	void scheduleMidnightRestart();
	void onMidnightStop();
	void onMidnightRestart();

	void showMsgBox(const QString &text);
	void showMainWin();
	void toggleAlwaysOnTopFlag();
	void showActivityWarnings();
	void setupIcon();
	void setupCentralWidget(Settings &settings, TimeTracker &timetracker);
	void shutdown();

protected:
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
	void closeEvent(QCloseEvent *event) override;

public:
	explicit MainWin(Settings &settings, TimeTracker &timetracker, QWidget *parent = nullptr);
	void start();

signals:
	void sendButtons(Button button);

public slots:
	void update();
	void iconActivated(QSystemTrayIcon::ActivationReason reason);
	void minToTray();
	void toggleAlwaysOnTop();
	void reactOnLockState(LockEvent event);
	void onAboutToQuit();
};

#endif // MAINWIN_H

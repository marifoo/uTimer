#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QString>
#include <QTimer>
#include "contentwidget.h"
#include "settings.h"
#include "types.h"
#include "idatabasemanager.h"

class MainWin : public QMainWindow
{
	Q_OBJECT
private:
	ContentWidget *content_widget_;
	QSystemTrayIcon *tray_icon_;
	const Settings & settings_;
	TimeTracker& timetracker_;
	IDatabaseManager& db_;

	bool warning_activity_shown_;
	bool warning_pause_shown_;
	bool was_active_before_autopause_;

	// Midnight forced-stop handling
	QTimer* midnight_timer_;

	void scheduleMidnightStop();
	void onMidnightStop();

	void showMsgBox(const QString &text);
	void showMainWin();
	void toggleAlwaysOnTopFlag();
	void showActivityWarnings();
	void setupIcon();
	void setupCentralWidget(Settings &settings, TimeTracker &timetracker);
	void shutdown(bool force_direct = false);

protected:
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
	void closeEvent(QCloseEvent *event) override;

public:
	explicit MainWin(Settings &settings, TimeTracker &timetracker, IDatabaseManager &db, QWidget *parent = nullptr);
	void start();

signals:
	void sendButtons(Button button);

public slots:
	void update();
	void iconActivated(QSystemTrayIcon::ActivationReason reason);
	void minToTray();
	void toggleAlwaysOnTop();
	void reactOnLockState(LockEvent event);
	void showUserWarning(const QString& text);
	void showHistoryLoadReconciliation(const QString& text);
	void onAboutToQuit();
};

#endif // MAINWIN_H

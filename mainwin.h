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
	std::unique_ptr<ContentWidget> content_widget_;
	std::unique_ptr<QSystemTrayIcon> tray_icon_;
	Settings & settings_;

	void updateTrayIconTooltip(QString activity, QString pause);

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

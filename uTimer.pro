TARGET = uTimer

HEADERS = \
   $$PWD/contentwidget.h \
   $$PWD/databasemanager.h \
   $$PWD/mainwin.h \
   $$PWD/timetracker.h \
   $$PWD/lockstatewatcher.h \
   $$PWD/settings.h \
   $$PWD/types.h \
   $$PWD/helpers.h \
   $$PWD/logger.h \
   $$PWD/historydialog.h

SOURCES = \
   $$PWD/contentwidget.cpp \
   $$PWD/databasemanager.cpp \
   $$PWD/main.cpp \
   $$PWD/mainwin.cpp \
   $$PWD/timetracker.cpp \
   $$PWD/lockstatewatcher.cpp \
   $$PWD/historydialog.cpp \
   $$PWD/settings.cpp \
   $$PWD/helpers.cpp \
   $$PWD/logger.cpp

INCLUDEPATH = \
    $$PWD/.

TEMPLATE = app

CONFIG += qt c++17

QT += widgets sql

# Platform-specific configuration
win32 {
    LIBS += -lUser32
}

linux {
    QT += dbus
    OBJECTS_DIR = build
    MOC_DIR = build
    RCC_DIR = build
}

RESOURCES += \
    icon.qrc

DISTFILES += \
    README.md \
	QT-LICENSE.md


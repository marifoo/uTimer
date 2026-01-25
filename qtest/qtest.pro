TARGET = qtest
TEMPLATE = app
CONFIG += qt console c++17
QT += testlib widgets sql

# Platform-specific configuration
win32 {
    LIBS += -lUser32
}
linux {
    QT += dbus
}

INCLUDEPATH += $$PWD/..

SOURCES += \
    utimertest.cpp \
    $$PWD/../timetracker.cpp \
    $$PWD/../lockstatewatcher.cpp \
    $$PWD/../databasemanager.cpp \
    $$PWD/../helpers.cpp \
    $$PWD/../settings.cpp \
    $$PWD/../logger.cpp

HEADERS += \
    utimertest.h \
    $$PWD/../timetracker.h \
    $$PWD/../lockstatewatcher.h \
    $$PWD/../databasemanager.h \
    $$PWD/../helpers.h \
    $$PWD/../settings.h \
    $$PWD/../logger.h \
    $$PWD/../types.h


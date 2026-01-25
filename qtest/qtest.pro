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
    main.cpp \
    testcommon.cpp \
    test_settings.cpp \
    test_helpers.cpp \
    test_lockstatewatcher.cpp \
    test_cleanduration.cpp \
    test_timetracker.cpp \
    test_database.cpp \
    test_integration.cpp \
    test_historydialog.cpp \
    utimertest.cpp \
    $$PWD/../historydialog.cpp \
    $$PWD/../timetracker.cpp \
    $$PWD/../lockstatewatcher.cpp \
    $$PWD/../databasemanager.cpp \
    $$PWD/../helpers.cpp \
    $$PWD/../settings.cpp \
    $$PWD/../logger.cpp

HEADERS += \
    testcommon.h \
    test_settings.h \
    test_helpers.h \
    test_lockstatewatcher.h \
    test_cleanduration.h \
    test_timetracker.h \
    test_database.h \
    test_integration.h \
    test_historydialog.h \
    utimertest.h \
    $$PWD/../timetracker.h \
    $$PWD/../lockstatewatcher.h \
    $$PWD/../databasemanager.h \
    $$PWD/../helpers.h \
    $$PWD/../settings.h \
    $$PWD/../logger.h \
    $$PWD/../types.h \
    $$PWD/../historydialog.h

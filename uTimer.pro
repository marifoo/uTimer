TARGET = uTimer

HEADERS = \
   $$PWD/contentwidget.h \
   $$PWD/mainwin.h \
   $$PWD/timetracker.h \
   $$PWD/lockstatewatcher.h \
   $$PWD/settings.h \
   $$PWD/types.h \
   $$PWD/helpers.h \
   $$PWD/logger.h

SOURCES = \
   $$PWD/contentwidget.cpp \
   $$PWD/main.cpp \
   $$PWD/mainwin.cpp \
   $$PWD/timetracker.cpp \
   $$PWD/lockstatewatcher.cpp \
   $$PWD/settings.cpp \
   $$PWD/helpers.cpp \
   $$PWD/logger.cpp

INCLUDEPATH = \
    $$PWD/.

TEMPLATE = app

CONFIG += qt c++14

QT += widgets

LIBS += -lUser32

RESOURCES += \
    icon.qrc

DISTFILES += \
    README.md


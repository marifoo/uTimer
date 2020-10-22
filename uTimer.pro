# Created by and for Qt Creator This file was created for editing the project sources only.
# You may attempt to use it for building too, by modifying this file here.

TARGET = uTimer

HEADERS = \
   $$PWD/contentwidget.h \
   $$PWD/mainwin.h \
   $$PWD/timetracker.h \
   lockstatewatcher.h \
   settings.h \
   types.h

SOURCES = \
   $$PWD/contentwidget.cpp \
   $$PWD/main.cpp \
   $$PWD/mainwin.cpp \
   $$PWD/timetracker.cpp \
   lockstatewatcher.cpp \
   settings.cpp

INCLUDEPATH = \
    $$PWD/.

TEMPLATE = app

CONFIG += qt c++14

QT += widgets

LIBS += -lUser32

QMAKE_CXXFLAGS += -Wall -Wextra

RESOURCES += \
    icon.qrc

DISTFILES += \
    README.md


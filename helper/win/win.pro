TEMPLATE = app

include($$top_srcdir/deployment.pri)

QT += core network

LIBS += -llzma

CONFIG += c++11
CONFIG += console

TARGET = helper

target.path = $$LIBEXECDIR
INSTALLS += target

SOURCES = main.cpp \
    writejob.cpp \
    restorejob.cpp

HEADERS += \
    writejob.h \
    restorejob.h

RESOURCES += ../../translations/translations.qrc

DISTFILES += \
    helper.exe.manifest

QMAKE_MANIFEST = $${PWD}/helper.exe.manifest

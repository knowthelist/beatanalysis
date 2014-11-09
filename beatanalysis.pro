#-------------------------------------------------
#
# Project created by QtCreator 2014-11-04T20:06:06
#
#-------------------------------------------------

QT       += core gui


greaterThan(QT_MAJOR_VERSION, 4): {
    QT += widgets
    DEFINES += GST_API_VERSION_1
}

TARGET = beatanalysis
TEMPLATE = app


SOURCES += main.cpp\
        mainwindow.cpp \
    trackanalyser.cpp \
    player.cpp

HEADERS  += mainwindow.h \
    trackanalyser.h \
    player.h

FORMS    += mainwindow.ui

macx {
    DEFINES += GST_API_VERSION_1
    INCLUDEPATH += /usr/local/include/gstreamer-1.0 \
        /usr/local/include/glib-2.0 \
        /usr/local/lib/glib-2.0/include \
        /usr/local/include
    LIBS += -L/usr/local/lib \
        -lgstreamer-1.0 \
        -lglib-2.0 \
        -lgstfft-1.0 \
        -lgstbase-1.0 \
        -lgobject-2.0 \
        -framework CoreAudio \
        -framework CoreFoundation
}

unix:!macx {

contains(DEFINES, GST_API_VERSION_1) {
    CONFIG += link_pkgconfig \
        gstreamer-1.0
    PKGCONFIG += gstreamer-1.0 \
        taglib alsa
}
else {
    CONFIG += link_pkgconfig \
        gstreamer
    PKGCONFIG += gstreamer-0.10 \
        taglib alsa
}

}

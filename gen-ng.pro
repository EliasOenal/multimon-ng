TEMPLATE = app
CONFIG += console
CONFIG -= qt
CONFIG -= app_bundle
DEFINES += MAX_VERBOSE_LEVEL=1

isEmpty(PREFIX) {
 PREFIX = /usr/local/
}
TARGET = gen-ng
target.path = $$PREFIX/bin
INSTALLS += target

HEADERS += \
	gen.h


SOURCES += \
	gen.c \
	gen_dtmf.c \
	gen_sin.c \
	gen_zvei.c \
	gen_hdlc.c \
	gen_uart.c \
	gen_clipfsk.c \
	costabi.c

macx{
DEFINES += DUMMY_AUDIO
DEFINES += ONLY_RAW
DEFINES += CHARSET_UTF8
}

win32{
DEFINES += DUMMY_AUDIO
DEFINES += ONLY_RAW
DEFINES += WINDOWS
LIBS += -lwinmm
}


unix:linux-g++-32:!symbian:!macx{
DEFINES += CHARSET_UTF8
}

unix:linux-g++-64:!symbian:!macx{
DEFINES += CHARSET_UTF8
}

unix:linux-g++:!symbian:!macx{
DEFINES += CHARSET_UTF8
}

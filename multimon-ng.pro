TEMPLATE = app
CONFIG += console
CONFIG -= qt
CONFIG -= app_bundle
DEFINES += MAX_VERBOSE_LEVEL=1
QMAKE_CFLAGS += -std=gnu99
QMAKE_CFLAGS += -g # For profiling

isEmpty(PREFIX) {
 PREFIX = /usr/local/
}
TARGET = multimon-ng
target.path = $$PREFIX/bin
INSTALLS += target

HEADERS += \
    multimon.h \
    gen.h \
    filter.h \
    filter-i386.h

SOURCES += \
    unixinput.c \
    uart.c \
    pocsag.c \
    selcall.c \
    hdlc.c \
    demod_zvei1.c \
    demod_zvei2.c \
    demod_zvei3.c \
    demod_pzvei.c \
    demod_dzvei.c \
    demod_ccir.c \
    demod_eia.c \
    demod_eea.c \
    demod_ufsk12.c \
    demod_poc24.c \
    demod_poc12.c \
    demod_poc5.c \
    demod_hapn48.c \
    demod_fsk96.c \
    demod_dtmf.c \
    demod_clipfsk.c \
    demod_afsk24.c \
    demod_afsk24_3.c \
    demod_afsk24_2.c \
    demod_afsk12.c \
    costabi.c \
    costabf.c \
    clip.c \
    demod_eas.c \
    demod_morse.c \
    demod_dumpcsv.c


macx{
DEFINES += DUMMY_AUDIO
DEFINES += NO_X11
DEFINES += CHARSET_UTF8
#DEFINES += ARCH_X86_64
#LIBS += -lX11 -L/usr/X11R6/lib -R/usr/X11R6/lib # If you care you can also compile this on OSX. Though
                                                 # since Apple will remove Xorg from Mountain Lion I feel
                                                 # like we should get rid of this dependency.
}

win32{
#DEFINES += DUMMY_AUDIO
DEFINES += WIN32_AUDIO
DEFINES += NO_X11
DEFINES += ONLY_RAW
DEFINES += WINDOWS
SOURCES += win32_soundin.c
LIBS += -lwinmm
#DEFINES += ARCH_I386
}

unix:freebsd-g++:!symbian:!macx{
#DEFINES += ARCH_I386
DEFINES += PULSE_AUDIO
DEFINES += CHARSET_UTF8
LIBS += -L/usr/local/lib -LX11 -lpulse-simple -lpulse
SOURCES +=  xdisplay.c \
            demod_display.c
}

unix:linux-g++-32:!symbian:!macx{
#DEFINES += ARCH_I386
DEFINES += PULSE_AUDIO
DEFINES += CHARSET_UTF8
LIBS += -lX11 -lpulse-simple -lpulse
SOURCES +=  xdisplay.c \
            demod_display.c
}

unix:linux-g++-64:!symbian:!macx{
#DEFINES += ARCH_X86_64
DEFINES += PULSE_AUDIO
DEFINES += CHARSET_UTF8
LIBS += -lX11 -lpulse-simple -lpulse
SOURCES +=  xdisplay.c \
            demod_display.c
}

unix:linux-g++:!symbian:!macx{
DEFINES += PULSE_AUDIO
DEFINES += CHARSET_UTF8
LIBS += -lX11 -lpulse-simple -lpulse
SOURCES +=  xdisplay.c \
            demod_display.c
}

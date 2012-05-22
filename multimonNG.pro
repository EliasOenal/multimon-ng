TEMPLATE = app
CONFIG += console
CONFIG -= qt

HEADERS += \
    multimon.h \
    gen.h \
    filter.h \
    filter-i386.h

SOURCES += \
    unixinput.c \
    uart.c \
    pocsag.c \
    hdlc.c \
    demod_zvei.c \
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
    demod_eas.c

macx{
DEFINES += DUMMY_AUDIO
DEFINES += NO_X11
DEFINES += ARCH_X86_64
#LIBS += -lX11 -L/usr/X11R6/lib -R/usr/X11R6/lib # If you care you can also compile this on OSX. Though
                                                 # since Apple will remove Xorg from Mountain Lion I feel
                                                 # like we should get rid of this dependency.
}

win32{
DEFINES += DUMMY_AUDIO
DEFINES += NO_X11
DEFINES += ARCH_I386
}


unix:linux-g++-32:!symbian:!macx{
DEFINES += ARCH_I386
LIBS += -lX11 -L/usr/X11R6/lib -R/usr/X11R6/lib
SOURCES +=  xdisplay.c \
            demod_display.c
}

unix:linux-g++-64:!symbian:!macx{
DEFINES += ARCH_X86_64
LIBS += -lX11 -L/usr/X11R6/lib -R/usr/X11R6/lib
SOURCES +=  xdisplay.c \
            demod_display.c
}

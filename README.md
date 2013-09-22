multimon-ng a fork of multimon. It decodes the following digital transmission modes:

- POCSAG512 POCSAG1200 POCSAG2400
- EAS
- UFSK1200 CLIPFSK AFSK1200 AFSK2400 AFSK2400_2 AFSK2400_3
- HAPN4800
- FSK9600 
- DTMF
- ZVEI1 ZVEI2 ZVEI3 DZVEI PZVEI
- EEA EIA CCIR
- MORSE CW

The following changes have been made so far:
- Fixes for x64
- Basic functionality on Mac OS X 'Lion' (Soundcard/OSS input is unsupported)
- `DUMMY_AUDIO` "backend" (Gets rid of the OSS dependency, breaks audio in doing so)
- `ONLY_RAW` disables the format conversion while getting rid of posix dependencies
- Option `NO_X11` to disable the X11 dependency since Apple will drop Xorg soon
- Override mode for POCSAG decoding (e.g. force text decoding)
- Brute-Force BCH implementation for POCSAG forward error correction
- Verbose mode is now listed in `-h`
- Continued EAS/SAME development. The decoder now works, but it should be considered "alpha" quality. Do not rely on it for the reception of emergency alerts!
- Portability is a major goal
- Compiles on Windows (MinGW or Cygwin) without format conversion
- PulseAudio support, contributed by inf_l00p_
- Windows native audio and a VisualStudio/MSVC project file, contributed by bzzt_ploink
- Now accepts raw samples as piped input

In addition to the deprecated legacy Makefile there is also a file for qmake which is the preferred way of building multimon-ng. It's recommended to use qmake to generate the Makefile.

```
mkdir build
cd build
qmake ../multimon-ng.pro
make
sudo make install
```

The installation prefix can be set by passing a 'PREFIX' parameter to qmake. e.g:
```qmake multimon-ng.pro PREFIX=/usr/local```

So far multimon-ng has been successfully built on OS X, Debian, Ubuntu and Windows.
(On Windows using the Qt-MinGW build environment, as well as Cygwin and VisualStudio/MSVC)

Files can be easily converted into multimon-ng's native raw format using *sox*. e.g:
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw pocsag_short.raw```
GNURadio can also generate the format using the file sink in input mode *short*. 

You can also "pipe" raw samples into multimon-ng using something like
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw - | ./multimon-ng -```
(note the trailing dash)

Packaging
---------

```
qmake multimon-ng.pro PREFIX=/usr/local
make
make install INSTALL_ROOT=/
```

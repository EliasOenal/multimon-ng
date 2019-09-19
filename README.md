multimon-ng is the successor of multimon. It decodes the following digital transmission modes:

- POCSAG512 POCSAG1200 POCSAG2400
- FLEX
- EAS
- UFSK1200 CLIPFSK AFSK1200 AFSK2400 AFSK2400_2 AFSK2400_3
- HAPN4800
- FSK9600 
- DTMF
- ZVEI1 ZVEI2 ZVEI3 DZVEI PZVEI
- EEA EIA CCIR
- MORSE CW
- X10

multimon-ng can be built using either qmake or CMake:
```
mkdir build
cd build
qmake ../multimon-ng.pro
make
sudo make install
```
```
mkdir build
cd build
cmake ..
make
sudo make install
```

The installation prefix can be set by passing a 'PREFIX' parameter to qmake. e.g:
```qmake multimon-ng.pro PREFIX=/usr/local```

So far multimon-ng has been successfully built on Arch Linux, Debian, Gentoo, Kali Linux, Ubuntu, OS X, Windows and FreeBSD.
(On Windows using the Qt-MinGW build environment, as well as Cygwin and VisualStudio/MSVC)

Files can be easily converted into multimon-ng's native raw format using *sox*. e.g:
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw pocsag_short.raw```
GNURadio can also generate the format using the file sink in input mode *short*. 

You can also "pipe" raw samples into multimon-ng using something like
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw - | ./multimon-ng -```
(note the trailing dash)

As a last example, here is how you can use it in combination with RTL-SDR:
```rtl_fm -f 403600000 -s 22050 | multimon-ng -t raw -a FMSFSK -a AFSK1200 /dev/stdin```

Packaging
---------

```
qmake multimon-ng.pro PREFIX=/usr/local
make
make install INSTALL_ROOT=/
```

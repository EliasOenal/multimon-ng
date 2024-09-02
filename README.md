# multimon-ng

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

## Building

multimon-ng can be built using either qmake or CMake:

#### qmake
```
mkdir build
cd build
qmake ../multimon-ng.pro
make
sudo make install
```

#### CMake
```
mkdir build
cd build
cmake ..
make
sudo make install
```

The installation prefix can be set by passing a 'PREFIX' parameter to qmake. e.g:
```qmake multimon-ng.pro PREFIX=/usr/local```

### Environments

So far multimon-ng has been successfully built on:

- Arch Linux
- Debian
- Gentoo
- Kali Linux
- Ubuntu
- OS X
- Windows (Qt-MinGW build environment, Cygwin, and VisualStudio/MSVC)
- FreeBSD

## Examples

### Wav to raw

Files can be easily converted into multimon-ng's native raw format using *sox*. e.g:

    sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw pocsag_short.raw

GNURadio can also generate the format using the file sink in input mode *short*. 

### Pipe sox to multimon-ng

You can also "pipe" raw samples into multimon-ng using something like:

    sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw - | ./multimon-ng -

> [!NOTE]
> Note the trailing dash, means write/read to/from stdin

### Pipe rtl_fm to multimon-ng

As a last example, here is how you can use it in combination with RTL-SDR:

    rtl_fm -f 403600000 -s 22050 | multimon-ng -t raw -a FMSFSK -a AFSK1200 /dev/stdin

### Flac record and parse live data

A more advanced sample that combines `rtl_fm`, `flac`, and `tee` to split the output from `rtl_rm` into separate streams. One stream to be passed to `flac` to record the audio and another stream to for example an application that does text parsing of `mulimon-ng` output



```sh
rtl_fm -s 22050 -f 123.456M -g -9.9 | tee >(flac -8 --endian=little --channels=1 --bps=16 --sample-rate=22050 --sign=signed - -o ~/recordings/rtlfm.$EPOCHSECONDS.flac -f) | multimon-ng -v 0 -a FLEX -a FLEX_NEXT -t raw /dev/stdin
```

1. You can pass `-l` to `rtl_fm` for the squelch level, this will cut the noise floor so less data gets encoded by flac and will significantly reduce the file size but could result in loss of signal data. **This value must be tuned!**
2. Flac uses `-8` here, if you run an a resource constraint device you may want to lower this value
3. The Flac `-o` argument value contains `$EPOCHSECONDS` to make unique files when this gets restarted

To replay the recorded flac file to multimon-ng (requires sox):

```sg
flac -d --stdout ~/recordings/rtlf/rtlfm.1725033204.flac | multimon-ng -v 0 -a FLEX_NEXT -t flac -
```

## Packaging

```
qmake multimon-ng.pro PREFIX=/usr/local
make
make install INSTALL_ROOT=/
```

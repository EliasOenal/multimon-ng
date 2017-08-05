#!/usr/bin/env python2
# -*- coding: utf-8 -*-
##################################################
# GNU Radio Python Flow Graph
# Title: Pager2
# Generated: Tue Aug  1 22:29:29 2017
##################################################

from gnuradio import analog
from gnuradio import audio
from gnuradio import blocks
from gnuradio import eng_notation
from gnuradio import filter
from gnuradio import gr
from gnuradio.eng_option import eng_option
from gnuradio.filter import firdes
from optparse import OptionParser
import osmosdr
import subprocess
import time


class pager2(gr.top_block):

    def __init__(self):
        do_audio = False
        gr.top_block.__init__(self, "Pager")

        ##################################################
        # Variables
        ##################################################
        self.volume = volume = 0.1
        self.sample_rate = sample_rate = 1e6
        self.out_scale = out_scale = 10000
        self.freq = freq = 148.664e6
        self.fm_sample = fm_sample = 500e3
        self.audio_rate = audio_rate = 48e3

        ##################################################
        # Blocks
        ##################################################
        if do_audio:
            self.volume_multiply = blocks.multiply_const_vff((volume, ))
        self.rational_resampler_xxx_0 = filter.rational_resampler_fff(
                interpolation=441,
                decimation=1000,
                taps=None,
                fractional_bw=None,
        )
        self.osmosdr_source_0 = osmosdr.source( args="numchan=" + str(1) + " " + 'hackrf' )
        self.osmosdr_source_0.set_sample_rate(sample_rate)
        self.osmosdr_source_0.set_center_freq(freq, 0)
        self.osmosdr_source_0.set_freq_corr(0, 0)
        self.osmosdr_source_0.set_dc_offset_mode(2, 0)
        self.osmosdr_source_0.set_iq_balance_mode(2, 0)
        self.osmosdr_source_0.set_gain_mode(False, 0)
        self.osmosdr_source_0.set_gain(0, 0)
        self.osmosdr_source_0.set_if_gain(32, 0)
        self.osmosdr_source_0.set_bb_gain(44, 0)
        self.osmosdr_source_0.set_antenna('', 0)
        self.osmosdr_source_0.set_bandwidth(0, 0)

        command = ["sox -t raw -esigned-integer -b16 -r 22050 - -esigned-integer -b16 -r 22050 -t raw - | /Users/oconnd1/projects/multimon-ng/build/multimon-ng -t raw -a POCSAG512 -a POCSAG1200 -a POCSAG2400 -e -u -f alpha --timestamp -"]
        #command = ["/Users/oconnd1/projects/multimon-ng/build/multimon-ng -t raw -a POCSAG512 -a POCSAG1200 -a POCSAG2400 -f alpha --timestamp -"]
        self.p = subprocess.Popen(command, shell = True, stdin = subprocess.PIPE)

        self.low_pass_filter_0 = filter.fir_filter_ccf(int(sample_rate / fm_sample), firdes.low_pass(
        	1, sample_rate, 7.5e3, 1.5e3, firdes.WIN_HAMMING, 6.76))
        self.blocks_float_to_short_0 = blocks.float_to_short(1, out_scale)
        self.blocks_fd_sink_0 = blocks.file_descriptor_sink(gr.sizeof_short*1, self.p.stdin.fileno())
        if do_audio:
            self.audio_sink_0 = audio.sink(22050, '', True)
        self.analog_nbfm_rx_0 = analog.nbfm_rx(
        	audio_rate=int(fm_sample / 10),
        	quad_rate=int(fm_sample),
        	tau=75e-6,
        	max_dev=5e3,
          )

        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_nbfm_rx_0, 0), (self.rational_resampler_xxx_0, 0))
        self.connect((self.blocks_float_to_short_0, 0), (self.blocks_fd_sink_0, 0))
        self.connect((self.low_pass_filter_0, 0), (self.analog_nbfm_rx_0, 0))
        self.connect((self.osmosdr_source_0, 0), (self.low_pass_filter_0, 0))
        self.connect((self.rational_resampler_xxx_0, 0), (self.blocks_float_to_short_0, 0))
        if do_audio:
            self.connect((self.rational_resampler_xxx_0, 0), (self.volume_multiply, 0))
            self.connect((self.volume_multiply, 0), (self.audio_sink_0, 0))

    def get_volume(self):
        return self.volume

    def set_volume(self, volume):
        self.volume = volume
        self.volume_multiply.set_k((self.volume, ))

    def get_sample_rate(self):
        return self.sample_rate

    def set_sample_rate(self, sample_rate):
        self.sample_rate = sample_rate
        self.osmosdr_source_0.set_sample_rate(self.sample_rate)
        self.low_pass_filter_0.set_taps(firdes.low_pass(1, self.sample_rate, 7.5e3, 1.5e3, firdes.WIN_HAMMING, 6.76))

    def get_out_scale(self):
        return self.out_scale

    def set_out_scale(self, out_scale):
        self.out_scale = out_scale
        self.blocks_float_to_short_0.set_scale(self.out_scale)

    def get_freq(self):
        return self.freq

    def set_freq(self, freq):
        self.freq = freq
        self.osmosdr_source_0.set_center_freq(self.freq, 0)

    def get_fm_sample(self):
        return self.fm_sample

    def set_fm_sample(self, fm_sample):
        self.fm_sample = fm_sample

    def get_audio_rate(self):
        return self.audio_rate

    def set_audio_rate(self, audio_rate):
        self.audio_rate = audio_rate


def main(top_block_cls=pager2, options=None):

    tb = top_block_cls()
    tb.start()
    try:
        raw_input('Press Enter to quit: ')
    except EOFError:
        pass
    tb.stop()
    tb.wait()


if __name__ == '__main__':
    main()

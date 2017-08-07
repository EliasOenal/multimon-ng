#!/usr/bin/env python
# -*- coding: utf-8 -*-
##################################################
# GNU Radio Python Flow Graph
# Title: Multipager
# Generated: Thu Aug  3 14:04:34 2017
##################################################

from gnuradio import analog
from gnuradio import audio
from gnuradio import blocks
from gnuradio import eng_notation
from gnuradio import filter
from gnuradio import gr
from gnuradio.eng_option import eng_option
from gnuradio.filter import firdes
from gnuradio.filter import pfb
from optparse import OptionParser
import osmosdr
import subprocess
import sip
import sys
import trollius as asyncio

samplefile = 'sampler.raw'
cmdpat = "sox -t raw -esigned-integer -b16 -r 22050 - -esigned-integer -b16 -r 22050 -t raw - | multimon-ng -t raw -q -a POCSAG512 -a POCSAG1200 -a POCSAG2400 -e -u -f alpha --timestamp --label \"%.3f MHz:\" -"

class multipager(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "Multipager")
        ##################################################
        # Variables
        ##################################################
        self.volume = volume = 0.5
        self.squelch = squelch = -20
        self.sample_rate = sample_rate = 1e6
        self.out_scale = out_scale = 10000
        self.num_chan = num_chan = 20
        self.freq = freq = 148.664e6
        self.decim = decim = 2
        self.channel = channel = 19
        self.audio_rate = audio_rate = 50e3

        ##################################################
        # Blocks
        ##################################################
        if True:
            self.source = blocks.file_source(gr.sizeof_gr_complex*1, samplefile, True)
        else:
            self.source = osmosdr.source(args="hackrf")
            self.source.set_sample_rate(sample_rate)
            self.source.set_center_freq(freq, 0)
            self.source.set_freq_corr(0, 0)
            self.source.set_dc_offset_mode(0, 0)
            self.source.set_iq_balance_mode(0, 0)
            self.source.set_gain_mode(False, 0)
            self.source.set_gain(0, 0)
            self.source.set_if_gain(36, 0)
            self.source.set_bb_gain(44, 0)
            self.source.set_antenna("", 0)
            self.source.set_bandwidth(0, 0)

        self.pfb_channelizer_ccf_0 = pfb.channelizer_ccf(
        	  num_chan,
        	  (firdes.low_pass(1.0, sample_rate/decim, 7.25e3, 1.5e3, firdes.WIN_HAMMING, 6.76)),
        	  1,
        	  60)
        self.pfb_channelizer_ccf_0.set_channel_map(([]))
        self.pfb_channelizer_ccf_0.declare_sample_delay(0)

        self.low_pass_filter_0 = filter.fir_filter_ccf(decim, firdes.low_pass(
        	1, sample_rate, 400e3, 500e3, firdes.WIN_HAMMING, 6.76))

        chwidth = sample_rate / decim / num_chan

        ##################################################
        # Connections
        ##################################################
        self.connect((self.source, 0), (self.low_pass_filter_0, 0))
        self.connect((self.low_pass_filter_0, 0), (self.pfb_channelizer_ccf_0, 0))

        # All channels
        sel = map(None, range(num_chan))

        loop = asyncio.get_event_loop()
        self.fms = {}
        for i in range(num_chan):
            #fifopath = fifopat % (i)
            fifopath = None
            if i > num_chan / 2:
                chfreq = freq + chwidth * (i - num_chan)
            else:
                chfreq = freq + chwidth * i

            if i in sel:
                print("Channel %d %.3f MHz" % (i, chfreq / 1e6))
                command = cmdpat % (chfreq / 1e6)
                fm = FMtoCommand(squelch, int(sample_rate / num_chan / decim), int(sample_rate / num_chan / decim), 5e3,
                                 out_scale, chfreq, command)

                self.connect((self.pfb_channelizer_ccf_0, i), (fm, 0))
                self.fms[chfreq] = fm
            else:
                n = blocks.null_sink(gr.sizeof_gr_complex*1)
                self.connect((self.pfb_channelizer_ccf_0, i), (n, 0))

class FMtoCommand(gr.hier_block2):
    def __init__(self, squelch, quad_rate, audio_rate, max_dev, out_scale, freq, command):
        gr.hier_block2.__init__(self, "FMtoCommand",
                                    gr.io_signature(1, 1, gr.sizeof_gr_complex),
                                    gr.io_signature(0, 0, gr.sizeof_gr_complex))

        analog_pwr_squelch = analog.pwr_squelch_cc(squelch, 1e-4, 0, True)
        analog_nbfm_rx = analog.nbfm_rx(
        	audio_rate = audio_rate,
            quad_rate = quad_rate,
        	tau = 75e-6,
        	max_dev = max_dev,
          )
        rational_resampler = filter.rational_resampler_fff(
            interpolation = 441,
            decimation = 500,
            taps = None,
            fractional_bw = None,
        )
        blocks_float_to_short = blocks.float_to_short(1, out_scale)

        self.p = subprocess.Popen(command, shell = True, stdin = subprocess.PIPE)
        sink = blocks.file_descriptor_sink(gr.sizeof_short*1, self.p.stdin.fileno())
        self.connect(self, (analog_pwr_squelch, 0))
        self.connect((analog_pwr_squelch, 0), (analog_nbfm_rx, 0))
        self.connect((analog_nbfm_rx, 0), (rational_resampler, 0))
        self.connect((rational_resampler, 0), (blocks_float_to_short, 0))
        self.connect((blocks_float_to_short, 0), (sink, 0))

def main(top_block_cls=multipager, options=None):

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

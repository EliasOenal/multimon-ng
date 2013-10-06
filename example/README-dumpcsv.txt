


To generate a png from a wav file

generate a .csv file from the data :

    multimon-ng -q -t wav -a DUMPCSV  x10rf.wav > x10rf.csv

use gnuplot to generate a PNG of the data

    gnuplot -e "plot_data_file='x10rf'" dumpcsv_png.txt

there should be a file named x10rf.png


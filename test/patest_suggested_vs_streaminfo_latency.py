#!/usr/bin/env python
"""

Run and graph the results of patest_suggested_vs_streaminfo_latency.c

Requires matplotlib for plotting: http://matplotlib.sourceforge.net/

"""
import os
from pylab import *
import numpy

testExeName = "PATest.exe" # rename to whatever the compiled patest_suggested_vs_streaminfo_latency.c binary is
dataFileName = 'patest_suggested_vs_streaminfo_latency.csv' # code below calls the exe to generate this file


def loadCsvData( dataFileName ):
    params= ""
    inputDevice = ""
    outputDevice = ""

    startLines = file(dataFileName).readlines(1024)
    for line in startLines:
        if "output device" in line:
            outputDevice = line.strip(" \t\n\r#")
        if "input device" in line:
            inputDevice = line.strip(" \t\n\r#")
    params = startLines[0].strip(" \t\n\r#")

    data = numpy.loadtxt(dataFileName, delimiter=",", skiprows=4).transpose()

    class R(object): pass
    result = R()
    result.titleInfo = '%s\n%s\n%s'%(params,inputDevice,outputDevice)
    result.suggestedLatency = data[0]
    result.halfDuplexOutputLatency = data[1]
    result.halfDuplexInputLatency = data[2]
    result.fullDuplexOutputLatency = data[3]
    result.fullDuplexInputLatency = data[4]
    return result;


os.system(testExeName + ' > ' + dataFileName)

d = loadCsvData(dataFileName)

plot( d.suggestedLatency, d.suggestedLatency )
plot( d.suggestedLatency, d.halfDuplexOutputLatency )
plot( d.suggestedLatency, d.halfDuplexInputLatency )
plot( d.suggestedLatency, d.fullDuplexOutputLatency )
plot( d.suggestedLatency, d.fullDuplexInputLatency )

title('PortAudio suggested (requested) vs. resulting (reported) stream latency\n%s'%d.titleInfo)
ylabel('PaStreamInfo::{input,output}Latency (s)')
xlabel('Pa_OpenStream suggestedLatency (s)')
grid(True)

show()

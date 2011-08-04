#!/usr/bin/env python
"""

Run and graph the results of patest_suggested_vs_streaminfo_latency.c

Requires matplotlib for plotting: http://matplotlib.sourceforge.net/

"""
import os
from pylab import *
import numpy
from matplotlib.backends.backend_pdf import PdfPages
pdfFile = PdfPages('patest_suggested_vs_streaminfo_latency.pdf')

testExeName = "PATest.exe" # rename to whatever the compiled patest_suggested_vs_streaminfo_latency.c binary is
dataFileName = 'patest_suggested_vs_streaminfo_latency.csv' # code below calls the exe to generate this file

inputDeviceIndex = -1 # -1 means default
inputDeviceIndex = -1 # -1 means default


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
    result.params = params
    for s in params.split(','):
        if "sample rate" in s:
            result.sampleRate = s

    result.inputDevice = inputDevice
    result.outputDevice = outputDevice
    result.suggestedLatency = data[0]
    result.halfDuplexOutputLatency = data[1]
    result.halfDuplexInputLatency = data[2]
    result.fullDuplexOutputLatency = data[3]
    result.fullDuplexInputLatency = data[4]
    return result;

# run the test with different frames per buffer values:

framesPerBufferValues = [0]
# powers of two
#for i in range (1,11):
#    framesPerBufferValues.append( 2 ^ i )

# could also test: multiples of 10, random numbers, powers of primes, etc

isFirst = True    

for framesPerBuffer in framesPerBufferValues:

    os.system(testExeName + " -1 -1 " + str(framesPerBuffer) + ' > ' + dataFileName)

    d = loadCsvData(dataFileName)

    if isFirst:
        figure(1)
        gcf().text(0.1, 0.0,
           'patest_suggested_vs_streaminfo_latency\n%s\n%s\n%s\n'%(d.inputDevice,d.outputDevice,d.sampleRate))
        pdfFile.savefig()
        isFirst = False
            
    figure(2)

    plot( d.suggestedLatency, d.suggestedLatency )
    plot( d.suggestedLatency, d.halfDuplexOutputLatency )
    plot( d.suggestedLatency, d.halfDuplexInputLatency )
    plot( d.suggestedLatency, d.fullDuplexOutputLatency )
    plot( d.suggestedLatency, d.fullDuplexInputLatency )

title('PortAudio suggested (requested) vs. resulting (reported) stream latency\n%s'%str(framesPerBufferValues))
ylabel('PaStreamInfo::{input,output}Latency (s)')
xlabel('Pa_OpenStream suggestedLatency (s)')
grid(True)

pdfFile.savefig()

pdfFile.close()

show()

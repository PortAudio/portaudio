# Make PortAudio for Silicon Graphics IRIX 6.2 - 6.5.
# Pieter suurmond, july 4, 2002. (for PA v18.)
# Based on SGI-specific sproc()-method to spawn children, not on POSIX-threads.

# Instead of "-lpthread", as with linux,
# just the audio- and math-library for SGI:
LIBS = -laudio -lm

CDEFINES = -I../pa_common

# Possible CFLAGS with MIPSpro compiler are: -32, -o32, -n32, -64,
# -mips1, -mips2, -mips3, -mips4, etc. Use -g, -g2, -g3 for debugging. 
# And use for example -O2 or -O3 for better optimization:
CFLAGS = -O2
PASRC = ../pa_common/pa_lib.c pa_sgi.c
PAINC = ../pa_common/portaudio.h

# Tests that work (on SGI Indy with R5000 @ 180MHz running IRIX 6.2): 
#TESTC = $(PASRC) ../pa_tests/patest_record.c    # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_many.c      # v18 ok (with PA_MIN_LATENCY_MSEC=250).
#TESTC = $(PASRC) ../pa_tests/patest_latency.c   # v18 ok (with PA_MIN_LATENCY_MSEC=250) patest_latency.c contained //
#TESTC = $(PASRC) ../pa_tests/patest_longsine.c  # v18 ok (with PA_MIN_LATENCY_MSEC=250).

TESTC = $(PASRC) ../pa_tests/patest_saw.c       # v18 ok (with PA_MIN_LATENCY_MSEC=250).
#TESTC = $(PASRC) ../pa_tests/patest_wire.c      # v18 seems ok.

#TESTC = $(PASRC) ../pa_tests/pa_devs.c          # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_sine.c      # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_sine_time.c # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_sine8.c     # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_leftright.c # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_pink.c      # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_clip.c      # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_stop.c      # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_dither.c    # TODO probably ok.
#TESTC = $(PASRC) ../pa_tests/patest_sync.c      # BEEPS and delta's, no crashes anymore with v15.

# Tests that were not yet performed:
#TESTC = $(PASRC) ../pa_tests/paqa_devs.c #  test??
#TESTC = $(PASRC) ../pa_tests/paqa_errs.c #  test??
#TESTC = $(PASRC) ../pa_tests/pa_fuzz.c   #  test??

TESTH   = $(PAINC)

all: patest

# "cc" for the MIPSpro compiler, may be changed to "gcc":
patest: $(TESTC) $(TESTH) Makefile
	cc $(CFLAGS) $(TESTC) $(CDEFINES) $(LIBS) -o patest

run: patest
	./patest


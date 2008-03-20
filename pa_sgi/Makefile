# Make PortAudio for Silicon Graphics IRIX (6.2)
# Pieter suurmond, august 17, 2001. (v15 pa_sgi sub-version #0.04)

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

# Tests that work                                    (SGI Indy with R5000 @ 180MHz running IRIX 6.2). 
#TESTC   = $(PASRC) ../pa_tests/patest_many.c      # OK
TESTC   = $(PASRC) ../pa_tests/patest_record.c    # OK
#TESTC   = $(PASRC) ../pa_tests/patest_latency.c   # OK
#TESTC   = $(PASRC) ../pa_tests/patest_longsine.c  # OK but needs more than 4 buffers to do without glitches.
#TESTC   = $(PASRC) ../pa_tests/patest_saw.c       # Seems OK (does it gracefully exit?).
#TESTC   = $(PASRC) ../pa_tests/patest_wire.c      # OK
#TESTC   = $(PASRC) ../pa_tests/pa_devs.c          # OK
#TESTC   = $(PASRC) ../pa_tests/patest_sine.c      # OK
#TESTC   = $(PASRC) ../pa_tests/patest_sine_time.c # OK
#TESTC   = $(PASRC) ../pa_tests/patest_sine8.c     # OK
#TESTC   = $(PASRC) ../pa_tests/patest_leftright.c # OK
#TESTC   = $(PASRC) ../pa_tests/patest_pink.c      # OK
#TESTC   = $(PASRC) ../pa_tests/patest_clip.c      # OK
#TESTC   = $(PASRC) ../pa_tests/patest_stop.c      # OK
#TESTC   = $(PASRC) ../pa_tests/patest_dither.c    # OK
#TESTC   = $(PASRC) ../pa_tests/patest_sync.c      # BEEPS and delta's, no crashes anymore.

# Tests that do not yet work.
#TESTC   = $(PASRC) ../pa_tests/paqa_devs.c        # Heavy crash with core-dump after PaHost_OpenStream: opening 1 output channel(s) on AL default
#TESTC   = $(PASRC) ../pa_tests/paqa_errs.c        # Heavy crash with core-dump after PaHost_OpenStream: opening 2 output channel(s) on AL default
#TESTC   = $(PASRC) ../pa_tests/pa_fuzz.c    # THIS CRASHED MY WHOLE IRIX SYSTEM after "ENTER"! :-(
                                             # PROCESS IN ITSELF RUNS OK, WITH LARGER BUFFSIZES THOUGH.
                                             # OH MAYBE IT WAS BECAUSE DEBUGGING WAS ON???
                                             # HMMM I'M NOT GONNA RUN THAT AGAIN... WAY TOO DANGEROUS!
TESTH   = $(PAINC)

all: patest

#(cc may be changed to gcc)
patest: $(TESTC) $(TESTH) Makefile
	cc $(CFLAGS) $(TESTC) $(CDEFINES) $(LIBS) -o patest

run: patest
	./patest


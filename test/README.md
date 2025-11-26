# PortAudio Tests

This directory contains various programs to test PortAudio. The files 
named patest_* are tests.

For more information on the TestPlan please visit:

https://github.com/PortAudio/portaudio/wiki/TestPlan

The current status is in:

https://github.com/PortAudio/portaudio/wiki/TestStatus
  
All of the tests are up to date with the V19 API. They should all compile
(without any warnings on GCC 3.3). Note that this does not necessarily mean that 
the tests pass, just that they compile.

Note that Phil Burk deleted the debug_* tests on 2/26/11. They were just hacked
versions of old V18 tests. If we need to debug then we can just hack a working V19 test.

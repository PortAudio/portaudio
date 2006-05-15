Hello

  This is a small list of steps in order to build portaudio
(Currently v19-devel) into a VC6 DLL and lib file.
This DLL contains all 3 current win32 PA APIS (MM/DS/ASIO)

1)Copy the source dirs that comes with the ASIO SDK inside pa_win\msvc
  so you should now have:
  
  pa_win\msvc\common
  pa_win\msvc\host
  pa_win\msvc\host\sample
  pa_win\msvc\host\pc
  pa_win\msvc\host\mac (not needed)
  
  You dont need "driver"
  

2)execure "make.bat", this assumes VC6 is installed in 
     C:\Program Files\Microsoft Visual Studio\
 
 if its not, 
  
  Open a command Prompt and execute "vcvars32.bat" which sets the environment
  so that you can use Microsoft's "nmake"  
  EX: C:\Program Files\Microsoft Visual Studio\VC98\Bin\vcvars32.bat
  or (C:\progra~1\micros~2\VC98\bin\vcvars32) dumb de dumb
  
  You should now have seen a line that said:
  "Setting environment for using Microsoft Visual C++ tools."
  While in pa_win\msvc , type "nmake makefile.msvc"  
  this _should_ create portaudio.dll and portaudio.lib 
 
3)Now in any VC6 project, in which you require portaudio,
  you can just link with portaudio.lib, and of course include the 
  relevant headers
  (portaudio.h, and/or pa_asio.h , pa_x86_plain_converters.h) See (*)
  
4) Your new exe should now use portaudio.dll.


Have fun!

(*): you may want to add/remove some DLL entry points.
Right now those 3 entries are _not_ from portaudio.h

(from portaudio.def)
(...)
PaAsio_GetAvailableLatencyValues    @50
PaAsio_ShowControlPanel             @51
PaUtil_InitializeX86PlainConverters @52


-----
last update April 16th 2003
David Viens, davidv@plogue.com
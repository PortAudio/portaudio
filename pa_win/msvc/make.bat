CALL C:\progra~1\micros~2\VC98\bin\vcvars32
del *.dll
del *.lib
nmake Makefile.msvc
del *.obj
del *.exp
del *.pch
del *.idb

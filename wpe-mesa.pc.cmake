prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
includedir=${prefix}/include
libdir=${exec_prefix}/lib

Name: wpe-mesa
Description: The mesa backend for the wpe library
Version: @WPE_MESA_VERSION@
Requires: wpe
Cflags: -I${includedir}
Libs: -L${libdir} -lWPEBackend-mesa

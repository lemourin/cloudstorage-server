AC_INIT([cloudstorage-server], [1.0])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_MACRO_DIRS([m4])
AC_USE_SYSTEM_EXTENSIONS
AC_LANG(C++)

LT_INIT([win32-dll shared disable-static])

AC_PROG_CXX

AX_CXX_COMPILE_STDCXX([14], [noext], [mandatory])

PKG_CHECK_MODULES([libjsoncpp], [jsoncpp])
PKG_CHECK_MODULES([libmicrohttpd], [libmicrohttpd])
PKG_CHECK_MODULES([libcloudstorage], [libcloudstorage])
PKG_CHECK_MODULES([libavutil], [libavutil])
PKG_CHECK_MODULES([libavformat], [libavformat])
PKG_CHECK_MODULES([libavcodec], [libavcodec])
PKG_CHECK_MODULES([libavfilter], [libavfilter])
PKG_CHECK_MODULES([libswscale], [libswscale])
PKG_CHECK_MODULES([libcurl], [libcurl])

case "${host_os}" in
  *mingw32*)
    SYS=mingw32
    ;;
  *)
    SYS=linux
    ;;
esac

AC_CONFIG_FILES([
  Makefile
  src/Makefile
])

SOCKET_LIBS=""
AS_IF([test "${SYS}" = "mingw32"], [
  SOCKET_LIBS="-lws2_32"
])
AC_SUBST(SOCKET_LIBS)

AC_OUTPUT

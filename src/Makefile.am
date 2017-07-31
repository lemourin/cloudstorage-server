ACLOCAL_AMFLAGS = -I m4

AM_CXXFLAGS = \
	$(libjsoncpp_CFLAGS) \
	$(libmicrohttpd_CFLAGS) \
	$(libcloudstorage_CFLAGS)

AM_LDFLAGS = \
	-no-undefined \
	$(SOCKET_LIBS)

bin_PROGRAMS = main
main_SOURCES = \
	main.cpp \
	Utility.cpp \
	HttpServer.cpp \
	DispatchServer.cpp \
	MicroHttpdServer.cpp

main_LDADD = \
	$(libjsoncpp_LIBS) \
	$(libmicrohttpd_LIBS) \
	$(libcloudstorage_LIBS)


ACLOCAL_AMFLAGS = -I m4

AM_CXXFLAGS = \
	$(libjsoncpp_CFLAGS) \
	$(libavutil_CFLAGS) \
	$(libffmpegthumbnailer_CFLAGS) \
	$(libcloudstorage_CFLAGS)

AM_LDFLAGS = \
	-no-undefined \
	$(SOCKET_LIBS)

bin_PROGRAMS = cloudstorage-server
cloudstorage_server_SOURCES = \
	main.cpp \
	Utility.cpp \
	HttpServer.cpp \
	DispatchServer.cpp

cloudstorage_server_LDADD = \
	$(libjsoncpp_LIBS) \
	$(libavutil_LIBS) \
	$(libffmpegthumbnailer_LIBS) \
	$(libcloudstorage_LIBS)


ACLOCAL_AMFLAGS = -I m4

AM_CXXFLAGS = \
	$(libjsoncpp_CFLAGS) \
	$(libmicrohttpd_CFLAGS) \
	$(libavutil_CFLAGS) \
	$(libffmpegthumbnailer_CFLAGS) \
	$(libcloudstorage_CFLAGS) \
	$(libcrypto_CFLAGS) \
	$(libcurl_CFLAGS)

AM_LDFLAGS = \
	-no-undefined \
	$(SOCKET_LIBS)

bin_PROGRAMS = cloudstorage-server
cloudstorage_server_SOURCES = \
	main.cpp \
	Utility.cpp \
	HttpServer.cpp \
	DispatchServer.cpp \
	MicroHttpdServer.cpp \
	CurlHttp.cpp

cloudstorage_server_LDADD = \
	$(libjsoncpp_LIBS) \
	$(libmicrohttpd_LIBS) \
	$(libavutil_LIBS) \
	$(libffmpegthumbnailer_LIBS) \
	$(libcloudstorage_LIBS) \
	$(libcrypto_LIBS) \
	$(libcurl_LIBS)


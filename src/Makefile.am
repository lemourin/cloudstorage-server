ACLOCAL_AMFLAGS = -I m4

AM_CXXFLAGS = \
	$(libjsoncpp_CFLAGS) \
	$(libavutil_CFLAGS) \
	$(libavcodec_CFLAGS) \
	$(libavformat_CFLAGS) \
	$(libavfilter_CFLAGS) \
	$(libswscale_CFLAGS) \
	$(libcloudstorage_CFLAGS) \
	-DWITH_THUMBNAILER

AM_LDFLAGS = \
	-no-undefined \
	$(SOCKET_LIBS)

bin_PROGRAMS = cloudstorage-server
cloudstorage_server_SOURCES = \
	main.cpp \
	Utility.cpp \
	HttpServer.cpp \
	DispatchServer.cpp \
	GenerateThumbnail.cpp

cloudstorage_server_LDADD = \
	$(libjsoncpp_LIBS) \
	$(libavutil_LIBS) \
	$(libavformat_LIBS) \
	$(libavcodec_LIBS) \
	$(libavfilter_LIBS) \
	$(libswscale_LIBS) \
	$(libcloudstorage_LIBS)


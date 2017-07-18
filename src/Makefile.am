ACLOCAL_AMFLAGS = -I m4

maindir = $(datadir)/resources

AM_CXXFLAGS = \
	$(libjsoncpp_CFLAGS) \
	$(libmicrohttpd_CFLAGS) \
	$(libcloudstorage_CFLAGS) \
	-DRESOURCE_PATH=\"$(maindir)\"

AM_LDFLAGS = \
	-no-undefined \
	$(SOCKET_LIBS)

bin_PROGRAMS = main
main_SOURCES = \
	main.cpp \
	Utility.cpp \
	GenericRequest.cpp \
	HttpServer.cpp \
	AuthorizationServer.cpp

main_DATA = \
	../resources/mega_login.html \
	../resources/mega_success.html \
	../resources/owncloud_login.html \
	../resources/owncloud_success.html \
	../resources/amazons3_login.html \
	../resources/amazons3_success.html \
	../resources/default_success.html \
	../resources/default_error.html

main_LDADD = \
	$(libjsoncpp_LIBS) \
	$(libmicrohttpd_LIBS) \
	$(libcloudstorage_LIBS)


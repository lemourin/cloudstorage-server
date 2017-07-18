ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src

EXTRA_DIST = \
	Doxyfile.in

DISTCLEANFILES = Doxyfile

Doxyfile: Doxyfile.in $(top_builddir)/config.status
	cd $(top_builddir) && $(SHELL) ./config.status --file=$(subdir)/$@

doc: Doxyfile
	doxygen

PREFIX = /usr/local

CDEBUGFLAGS = -Os -g -Wall

DEFINES = $(PLATFORM_DEFINES)

CFLAGS = $(CDEBUGFLAGS) $(DEFINES) $(EXTRA_DEFINES)

all: ahcpd ahcp-generate-address ahcp-generate

ahcpd: ahcpd.o message.o config.o lease.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o ahcpd \
              ahcpd.o message.o config.o lease.o $(LDLIBS)

ahcp-generate: ahcp-generate.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o ahcp-generate ahcp-generate.o $(LDLIBS)

ahcp-generate-address: ahcp-generate-address.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o ahcp-generate-address ahcp-generate-address.o $(LDLIBS)

ahcpd.html: ahcpd.man
	groff -man -Thtml ahcpd.man > ahcpd.html

ahcp-generate.html: ahcp-generate.man
	groff -man -Thtml ahcp-generate.man > ahcp-generate.html

ahcp-generate-address.html: ahcp-generate-address.man
	groff -man -Thtml ahcp-generate-address.man > ahcp-generate-address.html

.PHONY: install

install: all
	-rm -f $(TARGET)$(PREFIX)/bin/ahcpd $(TARGET)$(PREFIX)/bin/ahcp-generate-address
	mkdir -p $(TARGET)$(PREFIX)/bin/
	cp ahcpd ahcp-generate ahcp-generate-address $(TARGET)$(PREFIX)/bin/
	chmod +x $(TARGET)$(PREFIX)/bin/ahcpd
	chmod +x $(TARGET)$(PREFIX)/bin/ahcp-generate
	chmod +x $(TARGET)$(PREFIX)/bin/ahcp-generate-address
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-config.sh
	cp ahcp-config.sh $(TARGET)$(PREFIX)/bin/
	chmod +x $(TARGET)$(PREFIX)/bin/ahcp-config.sh
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-dummy-config.sh
	cp ahcp-dummy-config.sh $(TARGET)$(PREFIX)/bin/
	chmod +x $(TARGET)$(PREFIX)/bin/ahcp-dummy-config.sh
	mkdir -p $(TARGET)$(PREFIX)/man/man8/
	cp -f ahcpd.man $(TARGET)$(PREFIX)/man/man8/ahcpd.8
	cp -f ahcp-generate.man $(TARGET)$(PREFIX)/man/man8/ahcp-generate.8
	cp -f ahcp-generate-address.man $(TARGET)$(PREFIX)/man/man8/ahcp-generate-address.8

.PHONY: uninstall

uninstall:
	-rm -f $(TARGET)$(PREFIX)/bin/ahcpd
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-generate
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-generate-address
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-config.sh
	-rm -f $(TARGET)$(PREFIX)/bin/ahcp-dummy-config.sh
	-rm -f $(TARGET)$(PREFIX)/man/man8/ahcpd.8
	-rm -f $(TARGET)$(PREFIX)/man/man8/ahcp-generate.8
	-rm -f $(TARGET)$(PREFIX)/man/man8/ahcp-generate-address.8

.PHONY: clean

clean:
	-rm -f ahcpd ahcp-generate ahcp-generate-address
	-rm -f *.o *~ core TAGS gmon.out
	-rm -f ahcpd.thml ahcp-generate.html ahcp-generate-address.html

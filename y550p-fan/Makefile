CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=

PREFIX ?= /usr/local
SYSTEMD_UNIT_DIR ?= /etc/systemd/system

BINARY := y550p-fand
SERVICE := y550p-fan.service

.PHONY: all clean install uninstall status

all: $(BINARY)

$(BINARY): y550p-fand.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

install: $(BINARY)
	install -Dm0755 $(BINARY) $(PREFIX)/sbin/$(BINARY)
	install -Dm0644 $(SERVICE) $(SYSTEMD_UNIT_DIR)/$(SERVICE)
	systemctl daemon-reload
	systemctl enable $(SERVICE)
	systemctl restart $(SERVICE)

uninstall:
	-systemctl disable --now $(SERVICE)
	rm -f $(SYSTEMD_UNIT_DIR)/$(SERVICE)
	rm -f $(PREFIX)/sbin/$(BINARY)
	systemctl daemon-reload

status:
	systemctl --no-pager --full status $(SERVICE)
	cat /run/y550p-fan/status

clean:
	rm -f $(BINARY)

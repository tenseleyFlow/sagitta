override MODE := release
ifndef DESTDIR
DESTDIR = /usr/local
endif
first:
define INLINE
body
endef
second:
ifeq ($(MODE),release)
endif
third:
-include third.mk
fourth:
.PHONY: fifth
fifth:
# after target comment
install: all
	@printf '%s\n' "$(DESTDIR)"
next: dep # tail
VALUE = $(wildcard src/*) $(LONG) $@ $A
ifeq ($(MODE),release)
-include release.mk
endif

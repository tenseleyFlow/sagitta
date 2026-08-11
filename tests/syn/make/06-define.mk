define SCRIPT $(wildcard src/*) $(LONG) $@ $A # comment
  leading spaces are legal here
	printf $(wildcard src/*) $(NAME) $@ $A
endef

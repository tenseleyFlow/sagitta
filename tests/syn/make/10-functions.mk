SRC := $(wildcard src/*.c)
OBJ := $(patsubst %.c,%.o,$(SRC))

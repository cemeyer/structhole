CFLAGS+=	-Werror -Wextra -Wno-missing-field-initializers -Wpointer-arith \
			-Wcast-qual -Wwrite-strings -Wshadow -Wunused-parameter -Wcast-align \
			-Wformat=2

LDLIBS=	-lelf -ldw
all: structhole

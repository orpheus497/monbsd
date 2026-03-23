CC = cc
CFLAGS = -Wall -Wextra -Werror -Iinclude -O2

TARGET = monbsd
SRCS = src/main.c 
OBJS = ${SRCS:.c=.o}

all: ${TARGET}

${TARGET}: ${OBJS}
		${CC} ${CFLAGS} -o $@ ${OBJS}

.c.o:
		${CC} ${CFLAGS} -c $< -o $@

clean:
		rm -f ${TARGET} ${OBJS}

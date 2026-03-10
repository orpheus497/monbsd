CC?= cc
CFLAGS+= -Wall -Wextra -O2
PREFIX?= /usr/local
BINDIR= ${PREFIX}/bin
MANDIR= ${PREFIX}/man/man8

TARGET= monbsd
SRC= src/monbsd.c

TESTS= tests/test_aperf tests/test_compile tests/test_cpuctl tests/test_cpuid tests/test_pci tests/test_statfs tests/test_uptime

all: ${TARGET}

${TARGET}: ${SRC}
	${CC} ${CFLAGS} ${SRC} -o ${TARGET}

tests: ${TESTS}

.for _test in ${TESTS}
${_test}: ${_test}.c
	${CC} ${CFLAGS} ${_test}.c -o ${_test}
.endfor

clean:
	rm -f ${TARGET} ${TESTS}

purge: uninstall clean
	@echo "All monbsd binaries and man pages purged from system paths."

uninstall-user:
	@if [ -n "$$HOME" ]; then \
		rm -f "$$HOME/.local/bin/${TARGET}"; \
		echo "monbsd removed from $$HOME/.local/bin/."; \
	else \
		echo "\$$HOME is not set or empty; skipping user uninstall."; \
	fi

.PHONY: all clean install uninstall uninstall-user tests purge

install: ${TARGET}
	mkdir -p ${BINDIR}
	mkdir -p ${MANDIR}
	install -m 4755 -o root -g wheel ${TARGET} ${BINDIR}/${TARGET}
	install -m 444 monbsd.8 ${MANDIR}/monbsd.8
	@echo "monbsd installed to ${BINDIR}/${TARGET} with setuid root."
	@echo "Man page installed to ${MANDIR}/monbsd.8."

uninstall:
	rm -f ${BINDIR}/${TARGET}
	rm -f ${MANDIR}/monbsd.8

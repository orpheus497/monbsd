CC?= cc
CFLAGS+= -Wall -Wextra -O2 -pthread
PREFIX?= /usr/local
BINDIR= ${PREFIX}/bin
MANDIR= ${PREFIX}/man/man8

TARGET= monbsd
SRC= src/monbsd.c

TEST_SRCS= tests/test_aperf.c tests/test_compile.c tests/test_cpuctl.c tests/test_cpuid.c tests/test_pci.c tests/test_statfs.c tests/test_uptime.c
TESTS= $(TEST_SRCS:.c=)

BENCH_SRCS= benchmarks/bench.c benchmarks/bench_getifaddrs.c benchmarks/bench_opt.c
BENCH= $(BENCH_SRCS:.c=)

all: ${TARGET}

${TARGET}: ${SRC}
	${CC} ${CFLAGS} ${SRC} -o ${TARGET} -pthread

tests: ${TESTS}

bench: ${BENCH}

.SUFFIXES: .c
.c:
	${CC} ${CFLAGS} $< -o $@

check: tests
	@pass=0; skip=0; fail=0; \
	for t in ${TESTS}; do \
		printf '== %s: ' "$$t"; \
		./$$t && rc=0 || rc=$$?; \
		if [ $$rc -eq 0 ]; then pass=$$((pass+1)); echo ok; \
		elif [ $$rc -eq 77 ]; then skip=$$((skip+1)); echo SKIP; \
		else fail=$$((fail+1)); echo FAIL; fi; \
	done; \
	echo "pass=$$pass skip=$$skip fail=$$fail"; \
	[ $$fail -eq 0 ]

clean:
	rm -f ${TARGET} ${TESTS} ${BENCH}

purge: uninstall clean
	@echo "All monbsd binaries and man pages purged from system paths."

install-user: ${TARGET}
	@if [ -n "$$HOME" ]; then \
		mkdir -p "$$HOME/.local/bin"; \
		install -m 755 ${TARGET} "$$HOME/.local/bin/${TARGET}"; \
		echo "installed (no setuid: MSR/PCI telemetry limited)"; \
	else \
		echo "\$$HOME is not set or empty; skipping user install."; \
	fi

uninstall-user:
	@if [ -n "$$HOME" ]; then \
		rm -f "$$HOME/.local/bin/${TARGET}"; \
		echo "monbsd removed from $$HOME/.local/bin/."; \
	else \
		echo "\$$HOME is not set or empty; skipping user uninstall."; \
	fi

.PHONY: all clean install uninstall install-user uninstall-user tests bench check purge

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

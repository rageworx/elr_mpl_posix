TLIB=libemrmempool.a
TDIR=lib
ODIR=obj
BDIR=bin

TARGET=${TDIR}/${TLIB}

all: prepare clean ${TARGET}
test1: prepare ${BDIR}/test1
test2: prepare ${BDIR}/test2
example: prepare ${BDIR}/example
example2: prepare ${BDIR}/example2

prepare:
	@mkdir -p ${ODIR}
	@mkdir -p ${TDIR}
	@mkdir -p ${BDIR}

clean:
	@rm -rf ${ODIR}/*.o
	@rm -rf ${TARGET}
	@rm -rf ${BDIR}/test*
	@rm -rf ${BDIR}/example

${ODIR}/elr_mpl_posix.o: src/elr_mpl_posix.c
	@g++ -c -Iinc $< -O2 -o $@

${TARGET}: ${ODIR}/elr_mpl_posix.o
	@ar -cr $@ $^
	@ranlib $@

${BDIR}/test1: test/test.c ${TARGET}
	@g++ -Iinc $< -Llib -lemrmempool -O2 -o $@

${BDIR}/test2: test/test.c src/elr_mpl_posix.c
	@g++ -DDEBUG -Iinc $^ -g3 -o $@

${BDIR}/example2: example/example.c src/elr_mpl_posix.c
	@g++ -Iinc $^ -g3 -o $@

${BDIR}/example: example/example.c
	@g++ -Iinc $< -Llib -lemrmempool -O2 -o $@


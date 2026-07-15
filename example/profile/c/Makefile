ASCEND_HOME_PATH	    ?= /usr/local/Ascend/ascend-toolkit/latest

COMPILER                    := $(if $(wildcard ${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ccec),${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ccec,${ASCEND_HOME_PATH}/bin/ccec)
HOST_COMPILER               := g++
COMPILER_FLAG               := -xcce -O2 -std=c++17 --cce-enable-sanitizer -g
HOST_COMPILER_FLAG          := -O2 -std=c++17
LINK_FLAG                   := --cce-fatobj-link --cce-enable-sanitizer
DAV_FLAG                    := --cce-aicore-arch=dav-c220-vec
ASCENDC_INC_FLAG            := -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface -I${ASCEND_HOME_PATH}/include
HOST_INC_FLAG               := -I${ASCEND_HOME_PATH}/include
LINK_LIBS                   := -L${ASCEND_HOME_PATH}/lib64 -lruntime -lascendcl -lstdc++

all: build

build: test_kernel.o main.o test_kernel.fatbin

test_kernel.o: test_kernel.cpp
	$(COMPILER) $(COMPILER_FLAG) $(DAV_FLAG) $(ASCENDC_INC_FLAG) -o $@ -c $^

main.o: main.cpp
	$(HOST_COMPILER) $(HOST_COMPILER_FLAG) $(HOST_INC_FLAG) -o $@ -c $^

test_kernel.fatbin: test_kernel.o main.o
	$(COMPILER) $(LINK_FLAG) $(DAV_FLAG) -o $@ $^ ${LINK_LIBS}

.PHONY: clean
clean:
	rm *.o test_kernel.fatbin

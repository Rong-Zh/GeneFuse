DIR_INC = ./inc
DIR_SRC = ./src
DIR_OBJ = ./obj
BINDIR=/usr/local/bin

SRC = $(wildcard ${DIR_SRC}/*.cpp)  
OBJ = $(patsubst %.cpp,${DIR_OBJ}/%.o,$(notdir ${SRC})) 

TARGET = genefuse

BIN_TARGET = ${TARGET}

CC = g++
HTS_ROOT = ./deps/htslib
HTS_STATIC_LIB = ${HTS_ROOT}/lib/libhts.a
CFLAGS = -std=c++20 -g -I${DIR_INC} -I${HTS_ROOT}/include -MMD -MP
LDFLAGS = -static
STATIC_LIBS = ${HTS_STATIC_LIB} -ldeflate -llzma -lbz2 -lz -lm -lpthread -ldl

${BIN_TARGET}:${OBJ} ${HTS_STATIC_LIB}
	$(CC) ${LDFLAGS} $(OBJ) ${STATIC_LIBS} -o $@
    
${DIR_OBJ}/%.o:${DIR_SRC}/%.cpp | make_obj_dir
	$(CC) $(CFLAGS) -O3 -c  $< -o $@
.PHONY:clean
clean:
	rm obj/*.o
	rm genefuse

make_obj_dir:
	@if test ! -d $(DIR_OBJ) ; \
	then \
		mkdir $(DIR_OBJ) ; \
	fi

install:
	install $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed."

-include ${OBJ:.o=.d}

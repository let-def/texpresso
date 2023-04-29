OBJECTS=sprotocol.o state.o fs.o incdvi.o myabort.o renderer.o engine_tex.o engine_pdf.o engine_dvi.o synctex.o sexp_parser.o

CC=gcc -O2 -ggdb -I. -fPIC -Wall -Idvi -I/opt/homebrew/include
LIBS= -L/opt/homebrew/lib -lmupdf -lm -lmupdf-third -lz -ljpeg -ljbig2dec -lharfbuzz -lfreetype -lopenjp2 -lgumbo -lSDL2
DIR=../build

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))

all: texpresso 

texpresso-standalone: $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) ../build/libmydvi.a
	$(MAKE) -C ../client
	$(CC) -o $@ $^ $(LIBS)

texpresso-debug-proxy: proxy.c
	$(CC) -o $@ $^

texpresso: $(DIR)/driver.o $(DIR)/loader.o $(DIR)/logo.o texpresso.so
	$(CC) -o $@ $^ $(LIBS)

texpresso.so: $(DIR)/main.o $(DIR_OBJECTS) ../build/libmydvi.a
	$(MAKE) -C dvi
	$(CC) -ldl -shared -o $@ $^ $(LIBS)
	killall -SIGUSR1 texpresso || true

$(DIR)/%.o: %.c
	$(CC) -c -o $@ $^

clean:
	rm -f $(DIR_OBJECTS) $(DIR)/*.o
	$(MAKE) -C dvi clean

../build/libmydvi.a:
	$(MAKE) -C dvi

.PHONY: all clean ../build/libmydvi.a

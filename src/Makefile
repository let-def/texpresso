OBJECTS=sprotocol.o state.o fs.o incdvi.o myabort.o renderer.o engine_tex.o engine_pdf.o engine_dvi.o synctex.o sexp_parser.o

DIR=../build

DIR_OBJECTS=$(foreach OBJ,$(OBJECTS),$(DIR)/$(OBJ))

all: texpresso-dev

texpresso: $(DIR)/driver.o $(DIR)/main.o $(DIR)/logo.o $(DIR_OBJECTS) ../build/libmydvi.a
	$(CC) -o $@ $^ $(LIBS)

texpresso-debug-proxy: proxy.c
	$(CC) -o $@ $^

texpresso-dev: $(DIR)/driver.o $(DIR)/loader.o $(DIR)/logo.o texpresso.so
	$(CC) -o $@ $^ $(LIBS)

texpresso.so: $(DIR)/main.o $(DIR_OBJECTS) ../build/libmydvi.a
	$(MAKE) -C dvi
	$(CC) -ldl -shared -o $@ $^ $(LIBS)
	killall -SIGUSR1 texpresso-dev || true

$(DIR)/%.o: %.c
	$(CC) -c -o $@ -Idvi/ $^

clean:
	rm -f $(DIR_OBJECTS) $(DIR)/*.o
	$(MAKE) -C dvi clean

../build/libmydvi.a:
	$(MAKE) -C dvi

.PHONY: all clean ../build/libmydvi.a

../Makefile.config:
	$(MAKE) -C .. Makefile.config

include ../Makefile.config

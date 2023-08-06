UNAME_S := $(shell uname -s)
CC := g++
target   := sdlarch
sources  := sdlarch.cpp glad.c
CFLAGS   := -fsanitize=address -fpermissive -std=c++11 -Wall -O1 -g3 -Ilibjuice/include -I../imgui/ -I../imgui/backends/
ifeq ($(UNAME_S),Darwin)
LFLAGS   := -static-libstdc++ -fsanitize=address
else
LFLAGS   := -static-libgcc -fsanitize=address
endif
LIBS     := -L/c/Users/johnk/Programming/sdlarch/libjuice/build -ljuice -luv -lws2_32
packages := sdl2

sources += sdlarch.cpp glad.c ../imgui/imgui.cpp ../imgui/backends/imgui_impl_sdl2.cpp ../imgui/backends/imgui_impl_opengl3.cpp
objects += build/sdlarch.o build/glad.o ../imgui/imgui.o ../imgui/imgui_draw.o ../imgui/imgui_demo.o ../imgui/imgui_tables.o ../imgui/imgui_widgets.o build/imgui_impl_sdl2.o build/imgui_impl_opengl3.o
ifneq ($(packages),)
    LIBS    += $(shell pkg-config --libs-only-l $(packages))
    LFLAGS  += $(shell pkg-config --libs-only-L --libs-only-other $(packages))
    #CFLAGS  += $(shell pkg-config --cflags $(packages))
	CFLAGS += -IC:/msys64/mingw64/include/SDL2
endif

.PHONY: all clean

all: $(target) 
clean:
	-rm -rf build
	-rm -f $(target)
	-rm -f juice.dll
	

$(target): Makefile $(objects) $(PWD)/juice.dll
	$(CC) $(LFLAGS) -o $@ $(objects) $(LIBS)

LIBJUICE_FILES := $(wildcard libjuice/*)
$(PWD)/juice.dll: $(LIBJUICE_FILES)
	cd libjuice; powershell ./build.bat
	cp libjuice/build/juice.dll .

build/%.o: %.cpp Makefile sam2.c
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

build/glad.o: glad.c Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

build/imgui_impl_sdl2.o : ../imgui/backends/imgui_impl_sdl2.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

build/imgui_impl_opengl3.o : ../imgui/backends/imgui_impl_opengl3.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

../imgui/imgui.o : ../imgui/imgui.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

../imgui/imgui_draw.o : ../imgui/imgui_draw.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

../imgui/imgui_tables.o : ../imgui/imgui_tables.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

../imgui/imgui_widgets.o : ../imgui/imgui_widgets.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

../imgui/imgui_demo.o : ../imgui/imgui_demo.cpp Makefile
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

#-include $(addprefix build/,$(sources:.c=.d))


PLUGIN   := hyprspan.so
CXXFLAGS ?= -O2 -g
CXXFLAGS += -shared -fPIC -std=c++26 -Wall -Wextra -Wno-unused-parameter
DEPS     := pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

all: $(PLUGIN)

$(PLUGIN): main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@ $$(pkg-config --cflags $(DEPS)) $$(pkg-config --libs xcb-xinerama)

clean:
	rm -f $(PLUGIN)

.PHONY: all clean

CXX = clang++
CXXFLAGS = -target x86_64-w64-windows-gnu -O2 -std=c++17 -nostdinc \
           -I/usr/include/wine/msvcrt \
           -I/usr/include/wine/windows \
           -I/usr/lib/clang/22/include \
           -D_WIN32 -D_WIN64 -Wno-dll-attribute-on-redeclaration

LDFLAGS = -L/usr/lib/wine/x86_64-windows \
          -fuse-ld=lld -shared -nostdlib \
          -Wl,-e,DllMain \
          -lkernel32 -luser32 -lmsvcrt

SRCS = dllmain.cpp camera_mod.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = dinput8.dll

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

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
STRIP = llvm-strip
DIST_DIR = dist
DIST_RELEASE = $(DIST_DIR)/release
DIST_ZIP = $(DIST_DIR)/Thief2014_ThirdPersonMod_v1.0.zip

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

dist: $(TARGET)
	@mkdir -p $(DIST_RELEASE)
	@cp $(TARGET) $(DIST_RELEASE)/$(TARGET)
	@$(STRIP) --strip-all $(DIST_RELEASE)/$(TARGET)
	@cp thief_camera.ini $(DIST_RELEASE)/thief_camera.ini
	@cp README_DIST.txt $(DIST_RELEASE)/README.txt
	@rm -f $(DIST_ZIP)
	@cd $(DIST_RELEASE) && zip -q -9 -r ../Thief2014_ThirdPersonMod_v1.0.zip $(TARGET) thief_camera.ini README.txt
	@echo "=========================================================="
	@echo " Distribution package generated successfully:"
	@echo " $(DIST_ZIP)"
	@echo " Loose release files: $(DIST_RELEASE)/"
	@echo "=========================================================="

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(DIST_DIR)

.PHONY: all clean dist

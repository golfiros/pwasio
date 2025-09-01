LIB_NAME := pwasio.dll
DRIVER_REG := Software\\ASIO\\pwasio

VERSION_MAJOR := 0
VERSION_MINOR := 0
VERSION_PATCH := 1

DIR_LIB := lib
DIR_BLD := .build
DIR_SRC := src

DIR_GUARD = mkdir -p $(@D)

CC := clang
LIBS := -lodbc32 -lole32 -luuid -lwinmm -lshlwapi
PKG_CONFIG := libpipewire-0.3
CFLAGS := -fPIC -Wextra -Wall -fno-strict-aliasing -std=gnu23
DEFNS := -D_REENTRANT -DLIB_NAME='"$(LIB_NAME)"' -DDRIVER_REG='"$(DRIVER_REG)"'
DEFNS += -DPWASIO_VERSION_MAJOR=$(VERSION_MAJOR)
DEFNS += -DPWASIO_VERSION_MINOR=$(VERSION_MINOR)
DEFNS += -DPWASIO_VERSION_PATCH=$(VERSION_PATCH)

ifeq ($(DEBUG),true)
CFLAGS += -O0 -g
DEFNS += -DDEBUG -D__WINESRC__
else
CFLAGS += -O3 
DEFNS += -DNDEBUG
endif

TARGET := $(DIR_LIB)/wine/x86_64-unix/$(LIB_NAME).so
HEADERS := $(wildcard $(DIR_SRC)/*.h)
BINARIES := $(patsubst $(DIR_SRC)/%.c, $(DIR_BLD)/%.o, $(wildcard $(DIR_SRC)/*.c))

WINEBUILD = winebuild
WINECC    = winegcc
WINETARGET := $(DIR_LIB)/wine/x86_64-windows/$(LIB_NAME)

WINDOWSINC := /usr/include/wine/windows

all:
	make $(TARGET)
	make $(WINETARGET)

$(DIR_BLD)/%.o: $(DIR_SRC)/%.c $(HEADERS)
	$(DIR_GUARD)
	$(CC) $(DEFNS) $(CFLAGS) $(shell pkg-config --cflags $(PKG_CONFIG)) -I$(WINDOWSINC) -c $< -o $@

$(TARGET): $(BINARIES)
	$(DIR_GUARD)
	$(WINECC) $^ -shared -m64 $(LIB_NAME).spec $(shell pkg-config --libs $(PKG_CONFIG)) $(LIBS) -o $@

$(WINETARGET): $(BINARIES)
	$(DIR_GUARD)
	$(WINEBUILD) -m64 --dll --fake-module -E $(LIB_NAME).spec $^ -o $@

clean:
	rm -rf $(DIR_BLD)
	rm -rf $(DIR_LIB)

.PHONY: all clean

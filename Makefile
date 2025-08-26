# Cross-platform Makefile for SFML (Windows/Unix)
# - Windows (cmd/PowerShell): links SFML via SFML_DIR (or SFML_INC/SFML_LIB)
# - Unix (Linux/macOS): uses pkg-config if available

# Common dirs (define BEFORE OS-specific command macros)
SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin

# Detect OS
ifeq ($(OS),Windows_NT)
  EXE := .exe
  # Commands for Windows cmd (deferred expansion so $(BUILD_DIR)/$(BIN_DIR) are resolved)
  MKDIR_BUILD = if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
  MKDIR_BIN   = if not exist "$(BIN_DIR)"   mkdir "$(BIN_DIR)"
  RMDIR_BUILD = if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
  RMDIR_BIN   = if exist "$(BIN_DIR)"   rmdir /S /Q "$(BIN_DIR)"
  CXX := g++
  # Ensure GNU make uses cmd.exe on Windows so batch IF works
  SHELL := cmd
  .SHELLFLAGS := /C

  # Convenience: stop running game.exe to avoid linker 'Access is denied' (exit code 5)
  .PHONY: stop
  stop:
	- taskkill /F /IM game.exe 2>nul || exit /B 0

  # ===== SFML configuration (Windows) =====
  # Point this to your MinGW SFML install root, e.g.:
  #   - MSYS2 MinGW64: SFML_DIR=C:/msys64/mingw64
  #   - Prebuilt SFML MinGW: SFML_DIR=C:/SFML-2.6.1
  SFML_DIR ?=
  SFML_INC ?=
  SFML_LIB ?=
  # Derive include/lib from SFML_DIR if provided
  ifneq ($(SFML_DIR),)
    SFML_INC := $(SFML_DIR)/include
    SFML_LIB := $(SFML_DIR)/lib
  endif
  # Autodetect common prebuilt SFML path if not provided
  ifeq ($(SFML_INC),)
    ifneq (,$(wildcard C:/SFML-2.6.1/include))
      SFML_INC := C:/SFML-2.6.1/include
    endif
  endif
  ifeq ($(SFML_LIB),)
    ifneq (,$(wildcard C:/SFML-2.6.1/lib))
      SFML_LIB := C:/SFML-2.6.1/lib
    endif
  endif
  # Add include/lib paths if set
  ifneq ($(SFML_INC),)
    CXXFLAGS += -I"$(SFML_INC)"
  endif
  ifneq ($(SFML_LIB),)
    LDFLAGS  += -L"$(SFML_LIB)"
  endif

  # Try to determine SFML bin (for runtime PATH)
  ifneq ($(SFML_DIR),)
    SFML_BIN := $(SFML_DIR)/bin
  endif
  ifeq ($(SFML_BIN),)
    # Common prebuilt path fallback
    ifneq (,$(wildcard C:/SFML-2.6.1/bin))
      SFML_BIN := C:/SFML-2.6.1/bin
    endif
  endif

  # Try to detect MSYS2 MinGW runtime bin for GCC DLLs (libstdc++-6.dll, libwinpthread-1.dll, etc.)
  ifeq ($(MINGW_BIN),)
    ifneq (,$(wildcard C:/msys64/mingw64/bin))
      MINGW_BIN := C:/msys64/mingw64/bin
    endif
  endif
  

  # Print what we detected (helps diagnose if -L is missing)
  $(info SFML_INC=$(SFML_INC))
  $(info SFML_LIB=$(SFML_LIB))
  $(info SFML_BIN=$(SFML_BIN))

  # Link SFML (MinGW build of SFML required) + comdlg32 for file dialogs
  LDLIBS := -lsfml-graphics -lsfml-window -lsfml-system -lcomdlg32

  # Pre-link command on Windows to ensure previous exe is removed
  PRELINK := del /F /Q "$(BIN_DIR)\\game$(EXE)" 2>nul || exit /B 0

  # Run command on Windows: prepend SFML and MinGW bin dirs to PATH
  # Important: escape % as %% for GNU Make so cmd sees a single %
  RUN_CMD := cmd /C "set PATH=$(SFML_BIN);$(MINGW_BIN);%%PATH%% && .\\bin\\game.exe"

.PHONY: package
package: $(TARGET)
	$(MKDIR_BIN)
	@echo Packaging DLLs into "$(BIN_DIR)" ...
	@if exist "assets" xcopy /E /I /Y "assets" "$(BIN_DIR)\\assets" >nul
	@if exist "$(SFML_BIN)" copy /Y "$(SFML_BIN)\\*.dll" "$(BIN_DIR)\\" >nul
	@REM Also copy SFML external dependency DLLs from extlibs if present (freetype/ogg/vorbis/flac/jpeg/png/zlib)
	@if exist "$(SFML_DIR)\\extlibs\\bin\\x64" copy /Y "$(SFML_DIR)\\extlibs\\bin\\x64\\*.dll" "$(BIN_DIR)\\" >nul
	@if exist "$(MINGW_BIN)" copy /Y "$(MINGW_BIN)\\*.dll" "$(BIN_DIR)\\" >nul
	@echo Done.

else
  EXE :=
  # Commands for Unix shells
  MKDIR_BUILD = mkdir -p "$(BUILD_DIR)"
  MKDIR_BIN   = mkdir -p "$(BIN_DIR)"
  RMDIR_BUILD = rm -rf "$(BUILD_DIR)"
  RMDIR_BIN   = rm -rf "$(BIN_DIR)"
  CXX := g++

  # Pre-link (noop or safe removal on Unix)
  PRELINK := rm -f "$(BIN_DIR)/game$(EXE)"

  # Run command on Unix
  RUN_CMD := "$(BIN_DIR)/game$(EXE)"

  # Dependency include guard for WSL/paths with spaces or special chars
  # Disable including .d files when CURDIR contains spaces or '!' which can break Make parsing
  INCLUDE_DEPS := 1
  empty :=
  space := $(empty) $(empty)
  ifneq (,$(findstring $(space),$(CURDIR)))
    INCLUDE_DEPS := 0
  endif
  ifneq (,$(findstring !,$(CURDIR)))
    INCLUDE_DEPS := 0
  endif
  ifeq ($(NO_DEPS),1)
    INCLUDE_DEPS := 0
  endif

  # Prefer pkg-config, fallback to plain libs
  PKG_CFLAGS := $(shell pkg-config --cflags sfml-graphics sfml-window sfml-system 2>/dev/null)
  PKG_LIBS   := $(shell pkg-config --libs   sfml-graphics sfml-window sfml-system 2>/dev/null)
  LDLIBS := $(if $(PKG_LIBS),$(PKG_LIBS),-lsfml-graphics -lsfml-window -lsfml-system)

endif

# Target
TARGET := $(BIN_DIR)/game$(EXE)

# OS-specific run command fallback (only if not already defined)
ifeq ($(RUN_CMD),)
  ifeq ($(OS),Windows_NT)
    RUN_CMD := .\\$(BIN_DIR)\\game$(EXE)
  else
    RUN_CMD := $(TARGET)
  endif
endif

# Sources/objects
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Flags
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unknown-pragmas
# Project include paths (headers in src/ and root)
CXXFLAGS += -I$(SRC_DIR) -I.
# Add pkg-config cflags on Unix if present
CXXFLAGS += $(PKG_CFLAGS)

# Default goal
.PHONY: all
all: $(TARGET)

# Ensure dirs exist
.PHONY: dirs
dirs: $(BUILD_DIR) $(BIN_DIR)

$(BUILD_DIR):
	$(MKDIR_BUILD)

$(BIN_DIR):
	$(MKDIR_BIN)

# Compile
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Link
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(PRELINK)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

# Run
.PHONY: run
run: $(TARGET)
	$(RUN_CMD)

# Rebuild convenience target
.PHONY: rebuild
rebuild: clean all

# Fix clock skew on WSL by removing stale dependency files and forcing rebuild
ifeq ($(OS),Windows_NT)
# Windows fix-skew (noop)
.PHONY: fix-skew
fix-skew:
	@echo Skipping fix-skew on Windows
else
# Unix fix-skew: remove stale dependency files causing "multiple target patterns"
.PHONY: fix-skew
fix-skew:
	@find $(BUILD_DIR) -name '*.d' -delete 2>/dev/null || true
endif

# Clean
.PHONY: clean
clean:
	- $(RMDIR_BUILD)
	- $(RMDIR_BIN)

# Include dependency files (guarded)
ifeq ($(INCLUDE_DEPS),1)
-include $(DEPS)
endif
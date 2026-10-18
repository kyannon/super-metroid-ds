# The SNES core: snes9x's libretro core plus smds_ext.cpp, built twice
# (sm-core-a.so for the game, sm-core-b.so for the hidden map copy: two
# libraries, so the two copies' globals stay apart).
#
#   make -C core SNES9X=../deps/snes9x [CXX=... CC=... OUT=...]

SNES9X  ?= ../deps/snes9x
OUT     ?= ../build/host
OBJDIR  := $(OUT)/core-obj
CORE_DIR := $(SNES9X)
include $(SNES9X)/libretro/Makefile.common

OPT      ?= -O3
FLAGS    := $(OPT) -fPIC -fvisibility=hidden -DRIGHTSHIFT_IS_SAR -D__LIBRETRO__ -DALLOW_CPU_OVERCLOCK -DHAVE_STDINT_H -DHAVE_STRINGS_H $(INCFLAGS) $(CORE_ARCH_FLAGS)
CXXFLAGS := $(FLAGS) -std=gnu++17 -fno-rtti -fno-exceptions -w
CFLAGS   := $(FLAGS) -w

# snes9x's NTSC filter (LGPL) is left out; ntsc_stub.cpp stands in for it
SRCS := $(SOURCES_CXX) $(filter-out %/snes_ntsc.c,$(SOURCES_C)) smds_ext.cpp ntsc_stub.cpp
OBJS := $(patsubst %,$(OBJDIR)/%.o,$(subst $(SNES9X)/,s9x/,$(SRCS)))

all: $(OUT)/sm-core-a.so $(OUT)/sm-core-b.so

$(OUT)/sm-core-%.so: $(OBJS) link.T
	$(CXX) -shared -Wl,--version-script=link.T -Wl,-soname,sm-core-$*.so -o $@ $(OBJS) $(CORE_LDFLAGS) -lm

$(OBJDIR)/s9x/%.cpp.o: $(SNES9X)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJDIR)/s9x/%.c.o: $(SNES9X)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJDIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: all
.SECONDARY:

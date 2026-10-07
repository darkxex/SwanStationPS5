# PSXS5 - included after SwanStation's Makefile.libretro by
# tools/build-swanstation.sh: packs the objects of the libretro build into an
# archive instead of linking a shared library.
# SPDX-License-Identifier: GPL-3.0-or-later

# xxhash's runtime CPU dispatch wants the AVX-512 headers, which the PS5 clang
# hides (it has no AVX-512). Drop it: xxhash.c already builds for AVX2, and
# pre-defining the dispatch header's include guard keeps XXH3_128bits plain.
OBJECTS := $(filter-out %xxh_x86dispatch.o,$(OBJECTS))
src/core/texture_replacements.o: CXXFLAGS += -DXXH_X86DISPATCH_H_13563687684

psxs5-archive: $(OBJECTS)
	$(AR) rcs $(TARGET) $(OBJECTS)

# SwanStationPS5 v2 - included after Beetle's Makefile by tools/build-beetle.sh: packs
# the objects of the shared-library build (which carries Beetle's own
# libretro-common: file streams, threads, strings, hashes) into an archive
# instead of linking a .so.
# SPDX-License-Identifier: GPL-3.0-or-later
SwanStationPS5-archive: $(OBJECTS)
	$(AR) rcs $(TARGET) $(OBJECTS)

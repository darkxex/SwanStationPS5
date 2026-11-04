# SwanStationPS5 - included after SwanStation's Makefile.libretro by tools/build-swanstation-pc.sh:
# packs the objects of the libretro build into an archive instead of linking a DLL.
# SPDX-License-Identifier: GPL-3.0-or-later

SwanStationPS5-archive: $(OBJECTS)
	$(AR) rcs $(TARGET) $(OBJECTS)

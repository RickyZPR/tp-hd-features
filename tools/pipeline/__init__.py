"""Helper modules for tools/build_assets.py (Dusklight HD Mods asset pipeline).

common    path setup, logging, file helpers, PNG write/read
rarcx     RARC helpers: rebuild an archive keeping its exact entry layout (resource indices), structural check
convert   HD archive -> GC archive members (hd2gc textures + MAT4 tag) + MAT4 0x14E->0x14C repack (mat4fix)
memfit    room memory-block (STAG MEM0) sizing for D_SB11 and its calibration against the GC disc
cave      Cave of Shadows (D_SB11) overlay, floors.json, Colossal Wallet icon
stamps    Obj_Stamp.arc overlay, stamp PNGs + manifest
validate  every output check printed in the final table

All modules only read the HD dump / GC extraction and write into the staging directory given to them.
"""

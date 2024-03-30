@echo off

python ../meson/meson.py setup build_release --wipe --backend=vs --buildtype=release -Dcpp_std=vc++17 -Dcpp_args=["'/D _XBOX_UWP','/D _XBOX_FMALLOC'"] -Dc_args=["'/D _XBOX_UWP','/D _XBOX_FMALLOC'"] -Db_pch=false -Dc_winlibs=[] -Dcpp_winlibs=[]
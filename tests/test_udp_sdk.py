#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nbdriver-udp-sdk-')as directory:
    p=Path(directory)
    for legacy in (False,True):
        command=[os.environ.get('CC','cc'),'-D_GNU_SOURCE','-std=c11','-Wall','-Wextra','-Werror','-Wl,--wrap=ioctl','-I'+str(root/'include'),'-I'+str(root/'sdk/include'),str(root/'tests/udp_sdk_check.c')]
        if legacy:
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'sdk/include'),'-c',str(root/'sdk/src/client.c'),'-o',str(p/'client.o')],check=True)
            command+=['-DTEST_LEGACY',str(p/'client.o')]
        binary=p/('legacy'if legacy else'single');subprocess.run(command+['-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
    cpp=p/'probe.cpp';cpp.write_text('#include "nbdriver.h"\nstatic_assert(sizeof(nbdriver_udp_packet)==2096);\nstatic_assert(sizeof(nbdriver_udp_status)==96);\nint main(){nbdriver_udp_status s{};return nbdriver_udp_capture_status(-1,&s)==-1?0:1;}\n')
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'sdk/include'),str(cpp),'-o',str(p/'cpp')],check=True);subprocess.run([str(p/'cpp')],check=True)

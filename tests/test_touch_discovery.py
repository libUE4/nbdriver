#!/usr/bin/env python3
"""Check the actual discovery prototype with ARM64 KCFI and iterator ownership."""
import argparse
import pathlib
import re
import subprocess
import tempfile

from test_touch_frame import function

HERE = pathlib.Path(__file__).resolve().parent
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct class { int unused; };
struct device { int refs; };
struct input_mt { int num_slots; };
struct input_dev { struct device dev; struct input_mt *mt; bool candidate; };
#define NBDRIVER_TOUCH_SLOTS 16
#define to_input_dev(p) ((struct input_dev *)(p))
int class_for_each_device(const struct class *, const struct device *, void *,
                          int (*)(struct device *, void *));
static void get_device(struct device *dev) { ++dev->refs; }
static void put_device(struct device *dev) { assert(dev->refs>1); --dev->refs; }
static bool nbdriver_touch_candidate(struct input_dev *dev) { return dev->candidate; }
static struct class input_class;
static struct input_mt mt = {10};
static struct input_dev panel = {{1}, &mt, true};
static int missing_symbol;
int class_for_each_device(const struct class *cls, const struct device *start,
                          void *data, int (*fn)(struct device *, void *)) {
    assert(cls==&input_class && !start);
    get_device(&panel.dev); /* iterator's temporary reference */
    int result=fn(&panel.dev,data);
    --panel.dev.refs;
    return result;
}
static unsigned long nbdriver_ksym_lookup_optional(const char *name) {
    if (!strcmp(name,"class_for_each_device"))
        return missing_symbol==1 ? 0 : (unsigned long)&class_for_each_device;
    if (!strcmp(name,"input_class"))
        return missing_symbol==2 ? 0 : (unsigned long)&input_class;
    assert(0); return 0;
}
'''
SUFFIX = r'''
int main(void) {
    struct input_dev *found=nbdriver_touch_find_device();
    assert(found==&panel && panel.dev.refs==2);
    put_device(&found->dev); assert(panel.dev.refs==1);
    puts("PASS discovery owns a reference after iterator exit and releases it");
    for (missing_symbol=1;missing_symbol<=2;++missing_symbol) {
        assert(!nbdriver_touch_find_device()); assert(panel.dev.refs==1);
    }
    puts("PASS absent symbols return null without acquiring a reference");
    missing_symbol=0;panel.candidate=false;
    assert(!nbdriver_touch_find_device() && panel.dev.refs==1);
    panel.candidate=true;mt.num_slots=1;
    assert(!nbdriver_touch_find_device() && panel.dev.refs==1);
    mt.num_slots=17;
    assert(!nbdriver_touch_find_device() && panel.dev.refs==1);
    puts("PASS unsupported panels do not leak iterator references");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=pathlib.Path,
                        default=HERE.parent/'src/nbdriver_touch.c')
    args = parser.parse_args()
    source = args.source.read_text()
    match = re.search(r'^typedef\s+[^;]*\bnbdriver_class_for_each_device_fn\b[^;]*;',
                      source, re.M)
    if not match:
        raise RuntimeError('discovery typedef missing')
    typedef = match.group()
    with tempfile.TemporaryDirectory(prefix='touch-discovery-') as directory:
        root = pathlib.Path(directory)
        prototype = root/'prototype.c'
        prototype.write_text('struct class; struct device;\n'
            'int class_for_each_device(const struct class *, const struct device *, void *, '
            'int (*)(struct device *, void *));\n'+typedef+'\n'
            'int discovery_call(nbdriver_class_for_each_device_fn fn) '
            '{ return fn(0,0,0,0); }\n')
        ir = subprocess.check_output(['clang-18', '--target=aarch64-linux-gnu',
            '-O2', '-fsanitize=kcfi', '-S', '-emit-llvm', str(prototype), '-o', '-'], text=True)
        values = re.findall(r'"kcfi"\(i32 (-?\d+)\)', ir)
        assert len(values)==1, values
        actual = int(values[0]) & 0xffffffff
        print(f'actual_discovery_kcfi=0x{actual:08x} live_kernel_type=0x24d06eeb', flush=True)
        assert actual==0x24d06eeb, 'CFI type mismatch matches captured device panic'
        print('PASS exact call-site KCFI type matches captured kernel target', flush=True)
        model = root/'model.c'
        model.write_text(PREFIX+typedef+'\n'+
            function(source,'nbdriver_touch_match_device')+'\n'+
            function(source,'nbdriver_touch_find_device')+'\n'+SUFFIX)
        executable = root/'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(model), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)]).returncode


if __name__=='__main__':
    raise SystemExit(main())

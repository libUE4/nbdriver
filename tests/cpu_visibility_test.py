#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/nbdriver_features.c').read_text()


def declaration(name):
    match = re.search(r'^(?:static [^\n]*\b' + re.escape(name) + r'\(|struct ' +
                      re.escape(name) + r'\s*\{)', source, re.M)
    if not match:
        raise RuntimeError(name)
    opening = source.index('{', match.start())
    position = opening + 1
    depth = 1
    while depth:
        depth += (source[position] == '{') - (source[position] == '}')
        position += 1
    if source[match.start():].startswith('struct '):
        position += 1
    return source[match.start():position]


prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define CPU_VIS_SEGMENT_NS 2000000000ull
#define CPU_VIS_WEIGHT_BITS 16u
#define CPU_VIS_WEIGHT_TOTAL (1u << CPU_VIS_WEIGHT_BITS)
#define min(a,b) ((a) < (b) ? (a) : (b))
static u64 div64_u64(u64 a, u64 b) { return a / b; }
static u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 d)
{ return (u64)(((__uint128_t)a * b) / d); }
struct seq_file { char *buf; size_t count; size_t size; };
static bool seq_has_overflowed(struct seq_file *m) { return m->count == m->size; }
struct task_struct;
'''

names = ['cpu_vis_clock', 'cpu_vis_thread', 'cpu_vis_args',
         'cpu_visibility_segment_pct', 'cpu_visibility_clock_pct', 'cpu_visibility_clock_total',
         'cpu_visibility_share', 'cpu_visibility_prepare_sample',
         'cpu_visibility_record_valid', 'cpu_visibility_replace', 'cpu_visibility_stat_fields',
         'cpu_visibility_rewrite', 'cpu_visibility_rewrite_schedstat', 'cpu_visibility_rewrite_sched']

main = r'''
static u64 visible(const struct cpu_vis_thread *t, u64 budget)
{ return t->syn_ns + cpu_visibility_share(budget, t->begin, t->end); }

static void clocks(void)
{
    struct cpu_vis_clock c = { .epoch_ns = 100, .pct_lo = 50, .pct_hi = 50, .phase = 15765 };
    assert(cpu_visibility_clock_total(&c, 100 + 21600ull * 1000000000) == 10800ull * 1000000000);
    u64 previous = cpu_visibility_clock_total(&c, 1900000100);
    c.base_ns = previous;
    c.epoch_ns = 1900000100;
    c.pct_lo = c.pct_hi = 100;
    assert(cpu_visibility_clock_total(&c, 2000000100) == previous + 100000000);
    c.pct_lo = 1;
    c.pct_hi = 10;
    c.base_ns = 0;
    c.epoch_ns = 1;
    u64 integral = 0;
    for (u64 i = 0; i < 1000; i++) {
        integral += CPU_VIS_SEGMENT_NS / 100 * cpu_visibility_segment_pct(&c, i);
        assert(cpu_visibility_clock_total(&c, 1 + (i+1)*CPU_VIS_SEGMENT_NS) == integral);
        assert(cpu_visibility_clock_pct(&c, 1+i*CPU_VIS_SEGMENT_NS) >= 1);
        assert(cpu_visibility_clock_pct(&c, 1+i*CPU_VIS_SEGMENT_NS) <= 10);
    }
    puts("通过：倍率切换、长时间计算、区间积分");
}

static void allocation(void)
{
    struct cpu_vis_thread old[2] = {
        { .number=1, .start_ns=1, .real_ns=10000000000ull },
        { .number=2, .start_ns=2, .real_ns=10000000000ull },
    };
    assert(cpu_visibility_prepare_sample(NULL, 0, old, 2, 0) == 0);
    for (u64 n = 0; n < 200000; n += 17) {
        assert(visible(&old[0], n) + visible(&old[1], n) == n);
        assert(visible(&old[0], n+1) >= visible(&old[0], n));
        assert(visible(&old[1], n+1) >= visible(&old[1], n));
    }
    u64 budget = 50000000000ull;
    struct cpu_vis_thread next[2] = {old[0], old[1]};
    next[1].real_ns += 1000000000ull;
    assert(cpu_visibility_prepare_sample(old, 2, next, 2, budget) == 0);
    assert(next[0].syn_ns == visible(&old[0], budget));
    assert(next[1].syn_ns == visible(&old[1], budget));
    assert(visible(&next[0], 500000000) + visible(&next[1], 500000000) == budget+500000000);
    assert(visible(&next[0], 500000000) >= next[0].syn_ns);
    struct cpu_vis_thread survivor[1] = {next[0]};
    u64 retired = cpu_visibility_prepare_sample(next, 2, survivor, 1, 500000000);
    assert(survivor[0].syn_ns + retired == budget + 500000000);
    assert(visible(&survivor[0], 1000000000) - survivor[0].syn_ns == 1000000000);
    struct cpu_vis_thread reused[2] = {survivor[0], {.number=2,.start_ns=999,.real_ns=100}};
    assert(cpu_visibility_prepare_sample(survivor, 1, reused, 2, 1234) == 0);
    assert(reused[1].syn_ns == 0);
    assert(reused[0].syn_ns == survivor[0].syn_ns+1234);
    u64 big = UINT64_MAX / 2;
    assert(cpu_visibility_share(big, 0, 1) + cpu_visibility_share(big, 1, 65536) == big);
    puts("通过：分摊守恒、负载切换、线程退出与TID复用、单调性、64位预算");
}

static void bounded_text(void)
{
    size_t page = sysconf(_SC_PAGESIZE);
    char *buffer = mmap(NULL, 2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(buffer != MAP_FAILED);
    assert(mprotect(buffer+page, page, PROT_NONE) == 0);
    memset(buffer, 'X', page);
    struct seq_file m = { .buf=buffer, .size=page };
    struct cpu_vis_args args = { .m=&m };
    const char *stat = "12 (test (worker)) S 1 2 3 4 5 6 7 8 9 10 100 200 300 400\n";
    memcpy(buffer, stat, strlen(stat));
    m.count = strlen(stat);
    assert(cpu_visibility_rewrite(&args, 300000000000ull));
    char result[256];
    assert(m.count < sizeof(result));
    memcpy(result, buffer, m.count);
    result[m.count] = 0;
    assert(strstr(result, "200000000000 100000000000 300 400\n"));
    assert(cpu_visibility_rewrite(&args, 0));
    memcpy(result, buffer, m.count);
    result[m.count] = 0;
    assert(strstr(result, "10 0 0 300 400\n"));

    memcpy(buffer, "1 23 456\n", 9);
    m.count = 9;
    assert(cpu_visibility_rewrite_schedstat(&args, UINT64_MAX));
    assert(memcmp(buffer, "18446744073709551615 23 456\n", m.count) == 0);
    m.count = page;
    assert(!cpu_visibility_rewrite_schedstat(&args, 123));
    assert(!cpu_visibility_rewrite(&args, 123));
    assert(!cpu_visibility_rewrite_sched(&args, 123));

    const char *sched = "prefix\nse.sum_exec_runtime                 : 1.000000\nother : 7\n";
    memcpy(buffer, sched, strlen(sched));
    m.count = strlen(sched);
    assert(cpu_visibility_rewrite_sched(&args, 1234567890));
    memcpy(result, buffer, m.count);
    result[m.count] = 0;
    assert(strstr(result, "1234.567890\nother : 7\n"));

    memcpy(buffer, "pfx1 0 0\n", 9);
    m.count = 9;
    args.record_start = 3;
    assert(cpu_visibility_rewrite_schedstat(&args, 123));
    assert(memcmp(buffer, "pfx123 0 0\n", m.count) == 0);

    m.count = page-1;
    memset(buffer, 'X', page);
    buffer[page-1] = 'Z';
    assert(!cpu_visibility_replace(&m, 0, 1, "long", 4));
    assert(m.count == page);
    assert(buffer[page-1] == 'Z');
    munmap(buffer, 2*page);
    puts("通过：无NUL缓冲、字段增长与缩短、满缓冲、记录偏移、sched毫秒单位");
}

int main(void)
{
    clocks();
    allocation();
    bounded_text();
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='cpu-visibility-tests-') as directory:
    directory = Path(directory)
    code = directory / 'cpu_visibility.c'
    executable = directory / 'cpu_visibility'
    code.write_text(prefix + '\n'.join(declaration(name) for name in names) + main)
    subprocess.run([os.environ.get('CC', 'clang-18'), '-std=gnu11', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

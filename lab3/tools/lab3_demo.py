import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIELDS = (
    'initial_used_words', 'address_before', 'address_after', 'first_live_words',
    'checksum', 'live_words', 'collections_before_release', 'allocated_objects',
    'allocated_words', 'copied_objects', 'copied_words', 'reclaimed_before_release',
    'peak_words', 'root_visits', 'edge_visits', 'collections', 'final_used_words',
    'reclaimed_words',
)


def executable(value):
    return shutil.which(value) or str(Path(value).resolve())


def main():
    parser = argparse.ArgumentParser(description='Copying GC executed by MoarVM')
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--vm', default=str(ROOT / 'tools/moar.sh'))
    parser.add_argument('--heap-words', type=int, default=4096)
    parser.add_argument('--space', type=int, default=128)
    parser.add_argument('--roots', type=int, default=16)
    parser.add_argument('--rounds', type=int, default=200)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--output', type=Path, default=ROOT / 'output')
    args = parser.parse_args()
    if not 0 < args.timeout < float('inf') or not 0 <= args.rounds <= (1 << 63) - 1:
        parser.error('timeout must be positive and finite; rounds must fit nonnegative int64')
    if not 0 < args.heap_words <= (1 << 31) - 1 or args.space < 19 or args.roots < 5:
        parser.error('heap must fit positive int32; demo needs space >= 19 and roots >= 5')
    if 1 + 21 + args.roots + 2 * args.space > args.heap_words:
        parser.error('two semispaces, roots and GC state do not fit into the heap')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bytecode = output / 'copying.moarvm'
    compilation = [executable(args.compiler), '--heap-words', str(args.heap_words)]
    for value in (args.space, args.roots, args.rounds):
        compilation += ['--arg', str(value)]
    compilation += [str(bytecode), str(ROOT / 'runtime/gc.src'), str(ROOT / 'examples/copying.src')]
    subprocess.run(compilation, check=True, timeout=args.timeout)
    vm = executable(args.vm)
    result = subprocess.run([vm, str(bytecode)], check=True, text=True,
                            capture_output=True, timeout=args.timeout)
    values = [int(line) for line in result.stdout.splitlines()]
    if len(values) != len(FIELDS):
        raise ValueError('unexpected MoarVM demo output')
    report = dict(zip(FIELDS, values))
    if report['checksum'] != 579 or report['final_used_words'] != 0:
        raise ValueError('incorrect live graph or unreclaimed garbage')
    report['config'] = {'heap_words': args.heap_words, 'word_bytes': 8,
                        'semispace_words': args.space, 'root_capacity': args.roots,
                        'rounds': args.rounds, 'timeout_seconds': args.timeout}
    report['vm'] = subprocess.run([vm, '--version'], check=True, text=True,
                                  capture_output=True, timeout=args.timeout).stdout.strip()
    dump = subprocess.run([vm, '--dump', str(bytecode)], check=True, text=True,
                          capture_output=True, timeout=args.timeout)
    (output / 'copying.official-dump.txt').write_text(dump.stdout)
    (output / 'stats.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'stdout.txt').write_text(result.stdout)
    print(f'MoarVM: semispace={args.space} words, allocations in loop={args.rounds}')
    print(f"Root moved: {report['address_before']} -> {report['address_after']}")
    print(f"First collection: {report['initial_used_words']} -> {report['first_live_words']} words")
    print(f"Live graph: {report['live_words']} words; checksum={report['checksum']}")
    print(f"Collections: {report['collections']}; objects allocated: {report['allocated_objects']}")
    print(f"Copied: {report['copied_objects']} objects / {report['copied_words']} words")
    print(f"Reclaimed: {report['reclaimed_words']} words; final used: {report['final_used_words']} words")
    print(f'Bytecode, official MoarVM dump and statistics: {output}')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr, end='', file=sys.stderr)
        raise SystemExit(error.returncode)
    except (subprocess.TimeoutExpired, OSError, ValueError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)

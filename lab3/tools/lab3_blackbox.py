import argparse
import json
from pathlib import Path
import subprocess
import sys

from lab3_demo import ROOT, executable
from lab3_profile import parse_trace, plot


def reachable(graph, roots):
    visited = set()
    pending = list(roots)
    while pending:
        node = pending.pop()
        if node and node not in visited:
            visited.add(node)
            pending.extend(graph[node])
    return visited


def simulate(seed, steps, space, interval):
    graph, pool, resident = {}, [], set()
    events, snapshots, operations, collection_metrics = [], [], [], []
    allocated = collections = automatic = failures = cycle_writes = cyclic_reclaimed = 0
    requested = dict(allocate=0, link=0, release=0)
    completed = dict(allocate=0, link=0, release=0)

    def record(step, phase):
        live = reachable(graph, pool)
        event = dict(step=step, phase=phase, used_words=len(resident) * 6,
                     allocated=allocated, collections=collections, reclaimed_words=(allocated - len(resident)) * 6,
                     reachable_objects=len(live), resident_objects=len(resident), roots=len(pool))
        events.append(event)
        return event

    def snapshot(step, phase):
        live = reachable(graph, pool)
        snapshots.append(dict(step=step, phase=phase, event_index=len(events) - 1,
                              graph={node: list(graph[node]) for node in sorted(live)}))

    def collect():
        nonlocal resident, collections, cyclic_reclaimed
        live = reachable(graph, pool)
        collection_metrics.append(dict(surviving_objects=len(live), copied_objects=len(live),
                                       updated_roots=len(pool),
                                       updated_fields=sum(bool(target) for node in live for target in graph[node])))
        garbage = resident - live
        for node in garbage:
            if node in reachable(graph, graph[node]):
                cyclic_reclaimed += 1
        resident = live
        collections += 1

    record(0, 0)
    snapshot(0, 0)
    state = seed
    for step in range(1, steps + 1):
        draws = []
        for _ in range(4):
            state = state * 48271 % 2147483647
            draws.append(state)
        choice, first, second, field = draws[0] % 100, draws[1], draws[2], draws[3] % 2
        source = target = success = 0
        if choice < 35:
            kind, source = 1, step
            requested['allocate'] += 1
            if (len(resident) + 1) * 6 > space:
                collect()
                automatic += 1
            if (len(resident) + 1) * 6 <= space:
                graph[source] = [0, 0]
                resident.add(source)
                pool.append(source)
                allocated += 1
                completed['allocate'] += 1
                success = 1
            else:
                failures += 1
        elif choice < 65:
            kind = 2
            requested['link'] += 1
            if pool:
                source = pool[first % len(pool)]
                index = second % (len(pool) + 1)
                target = pool[index] if index < len(pool) else 0
                graph[source][field] = target
                if target and source in reachable(graph, [target]):
                    cycle_writes += 1
                completed['link'] += 1
                success = 1
        else:
            kind = 3
            requested['release'] += 1
            if pool:
                index = first % len(pool)
                source = pool[index]
                pool[index] = pool[-1]
                pool.pop()
                completed['release'] += 1
                success = 1
        operations.append([step, kind, source, target, field, success])
        record(step, kind)
        if step % interval == 0:
            snapshot(step, kind)
    snapshot(steps, 7)
    pool.clear()
    record(steps, 5)
    snapshot(steps, 5)
    return dict(events=events, snapshots=snapshots, operations=operations, collection_metrics=collection_metrics,
                summary=dict(requested=requested, completed=completed, allocation_failures=failures,
                             automatic_collections=automatic, explicit_collections=collections - automatic,
                             cycle_forming_writes=cycle_writes, cyclic_objects_reclaimed=cyclic_reclaimed,
                             peak_reachable_objects=max(e['reachable_objects'] for e in events)))


def validate(stdout, seed, steps, space, interval):
    expected = simulate(seed, steps, space, interval)
    lines = iter(line for line in stdout.splitlines() if not line.startswith('TRACE,'))
    operations = iter(expected['operations'])
    snapshots = iter(expected['snapshots'])
    events = iter(expected['events'])
    actual_ops = actual_events = actual_snaps = 0
    finished = False
    for line in lines:
        if line == 'BLACKBOX GC: OK':
            if finished:
                raise ValueError('duplicate completion marker')
            finished = True
            continue
        if finished:
            raise ValueError('unexpected data after completion')
        parts = line.split(',')
        values = [int(v) for v in parts[1:]]
        if parts[0] == 'OP':
            if values != next(operations, None):
                raise ValueError('random operation differs from the independent model')
            actual_ops += 1
        elif parts[0] == 'STATE':
            event = next(events, None)
            keys = ('step', 'phase', 'used_words', 'allocated', 'collections', 'reclaimed_words')
            if event is None or values != [event[k] for k in keys]:
                raise ValueError(f'collector state differs from model: {line}')
            actual_events += 1
        elif parts[0] == 'SNAP':
            snapshot = next(snapshots, None)
            if snapshot is None or values != [snapshot['step'], snapshot['phase']]:
                raise ValueError('unexpected snapshot')
            graph = {}
            for row in lines:
                node = row.split(',')
                if node[0] == 'END':
                    if len(node) != 2 or int(node[1]) != len(graph):
                        raise ValueError('invalid snapshot size')
                    break
                if node[0] != 'NODE' or len(node) != 4:
                    raise ValueError('invalid snapshot node')
                identifier = int(node[1])
                if identifier in graph:
                    raise ValueError('duplicate object ID')
                graph[identifier] = [int(v) for v in node[2:]]
            else:
                raise ValueError('unterminated snapshot')
            if graph != snapshot['graph']:
                raise ValueError('reachable object IDs or references changed')
            actual_snaps += 1
        else:
            raise ValueError(f'unexpected output: {line}')
    if (not finished or actual_ops != len(expected['operations']) or
            actual_events != len(expected['events']) or actual_snaps != len(expected['snapshots'])):
        raise ValueError('incomplete black-box run')
    stats, samples = parse_trace(stdout, space, steps, demo=None, require_empty=False)
    final = expected['events'][-1]
    if (stats['collections'] != final['collections'] or stats['allocated_objects'] != final['allocated'] or
            stats['allocated_words'] != final['allocated'] * 6 or
            stats['peak_words'] != max(e['used_words'] for e in expected['events']) or
            stats['final_used_words'] != final['used_words'] or stats['reclaimed_words'] != final['reclaimed_words'] or
            samples[-1]['event'] != 'finish'):
        raise ValueError('trace differs from the independent model')
    expected_gc = []
    previous = expected['events'][0]
    for event in expected['events'][1:]:
        if event['collections'] > previous['collections']:
            live_after = event['resident_objects'] - (event['allocated'] - previous['allocated'])
            expected_gc.append((previous['used_words'], live_after * 6))
        previous = event
    observed_gc = [(a['used_words'], b['used_words']) for a, b in zip(samples, samples[1:])
                   if a['event'] == 'before_gc' and b['event'] == 'after_gc']
    if observed_gc != expected_gc:
        raise ValueError('a collection preserved the wrong number of objects')
    measured = [sample for sample in samples if sample['event'] == 'after_gc']
    if len(measured) != len(expected['collection_metrics']):
        raise ValueError('missing collection metrics')
    for sample, metrics in zip(measured, expected['collection_metrics']):
        if any(sample[key] != value for key, value in metrics.items()):
            raise ValueError('collection metrics differ from the independent graph model')
    stats.update(expected['summary'])
    stats['reclaimed_objects'] = stats['reclaimed_words'] // 6
    stats['verified_snapshots'] = actual_snaps
    stats['final_reachable_objects'] = final['reachable_objects']
    stats['pending_garbage_objects'] = final['resident_objects'] - final['reachable_objects']
    return dict(semispace_words=space, stats=stats, samples=samples,
                events=expected['events'], snapshots=expected['snapshots'], operations=expected['operations'])


def collect(args, space):
    directory = args.output / f'space{space}'
    directory.mkdir(parents=True, exist_ok=True)
    bytecode = directory / 'blackbox.moarvm'
    command = [executable(args.compiler), '--heap-words', str(args.heap_words),
               '--profile-capacity', str(3 * args.steps + 7)]
    for value in (space, args.steps, args.seed, args.interval):
        command += ['--arg', str(value)]
    command += [str(bytecode), str(ROOT / 'runtime/gc.src'), str(ROOT / 'examples/blackbox_gc.src')]
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=args.timeout)
    result = subprocess.run([executable(args.vm), str(bytecode)], check=True, capture_output=True,
                            text=True, timeout=args.timeout)
    (directory / 'stdout.txt').write_text(result.stdout)
    run = validate(result.stdout, args.seed, args.steps, space, args.interval)
    dump = subprocess.run([executable(args.vm), '--dump', str(bytecode)], check=True,
                          capture_output=True, text=True, timeout=args.timeout)
    (directory / 'blackbox.official-dump.txt').write_text(dump.stdout)
    run['root_capacity'] = 2 * (space // 6 + 1) + 1
    return run


def plot_blackbox(report, directory):
    plot(report, directory)
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator
    colors = {1: '#188650', 2: '#7960a5', 3: '#dd8520'}
    for run in report['runs']:
        size = run['semispace_words']
        events = run['events']
        x = list(range(len(events)))
        fig, ax = plt.subplots(figsize=(13, 6))
        occupied = [e['resident_objects'] for e in events]
        live = [e['reachable_objects'] for e in events]
        ax.step(x, occupied, where='post', color='#2466ad', label='Объекты в куче (включая мусор)')
        ax.fill_between(x, occupied, step='post', color='#2466ad', alpha=.09)
        ax.step(x, live, where='post', color='#188650', linestyle='--', label='Достижимые объекты (модель)')
        for phase, label, marker in ((1, 'Создание: попытка', '^'), (2, 'Запись ссылки: попытка', '.'),
                                     (3, 'Снятие корня: попытка', 'v')):
            indices = [i for i, e in enumerate(events) if e['phase'] == phase]
            ax.scatter(indices, [occupied[i] for i in indices], color=colors[phase], marker=marker,
                       s=22, label=label, zorder=4)
        indices = [i for i in range(1, len(events)) if events[i]['collections'] > events[i - 1]['collections']]
        ax.scatter(indices, [occupied[i] for i in indices], color='#c83336', marker='v', s=50,
                   label='GC выполнен в этом событии', zorder=5)
        ax.set(xlabel='Событие: случайная операция или снятие всех корней', ylabel='Количество объектов',
               ylim=(0, max(occupied + [1]) * 1.18))
        ax.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax.grid(axis='y', alpha=.2)
        ax.legend(loc='upper left', bbox_to_anchor=(0, 1.2), ncol=3, fontsize=9, frameon=False)
        fig.suptitle(f'Чёрный ящик: {size * 8} байт · seed {report["config"]["seed"]} · '
                     f'{report["config"]["rounds"]} операций', fontsize=16, fontweight='bold')
        fig.text(.08, .025, 'Разница между линиями — недостижимые объекты до GC. Все сборки автоматические.\n'
                 'Пустой пул даёт пропуски операций; отказ выделения не создаёт объект. Событие выделения включает автоматический GC.',
                 fontsize=9, color='#536170')
        fig.subplots_adjust(top=.73, bottom=.17, left=.08, right=.98)
        for extension in ('png', 'svg'):
            fig.savefig(directory / f'blackbox-events-space{size}.{extension}', dpi=180)
        plt.close(fig)

        snapshots = run['snapshots']
        fig, ax = plt.subplots(figsize=(13, 6))
        x = list(range(len(snapshots)))
        series = [('allocated', 'Создано всего', '#8b9c9c', 1),
                  ('reachable_objects', 'Достижимо', '#188650', 1),
                  ('resident_objects', 'В куче', '#2466ad', 1),
                  ('reclaimed_words', 'Освобождено GC всего', '#dd8520', 6)]
        for offset, (key, label, color, divisor) in enumerate(series):
            values = [events[s['event_index']][key] // divisor for s in snapshots]
            bars = ax.bar([i + (offset - 1.5) * .2 for i in x], values, width=.19, label=label, color=color)
            ax.bar_label(bars, fontsize=7, padding=2)
        labels = [('старт' if s['phase'] == 0 else 'до финала' if s['phase'] == 7 else
                   'корни сняты' if s['phase'] == 5 else str(s['step']))
                  for s in snapshots]
        ax.set_xticks(x, labels, rotation=35, ha='right')
        ax.set(xlabel='Контрольная точка (номер случайной операции)', ylabel='Количество объектов')
        ax.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax.margins(y=.2)
        ax.grid(axis='y', alpha=.2)
        ax.set_axisbelow(True)
        ax.legend(loc='upper left', bbox_to_anchor=(0, 1.12), ncol=4, fontsize=9, frameon=False)
        fig.suptitle(f'Чёрный ящик: проверенные снимки · полупространство {size * 8} байт',
                     fontsize=16, fontweight='bold')
        fig.text(.08, .025, 'В каждой точке идентификаторы и обе ссылки всех достижимых объектов сверены с независимой моделью Python.\n'
                 'Создано всего = в куче + освобождено GC всего. Отдельного освобождения по счётчикам ссылок здесь нет.',
                 fontsize=9, color='#536170')
        fig.subplots_adjust(top=.81, bottom=.24, left=.08, right=.98)
        for extension in ('png', 'svg'):
            fig.savefig(directory / f'blackbox-snapshots-space{size}.{extension}', dpi=180)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description='Random black-box test of the copying collector on MoarVM')
    parser.add_argument('--compiler', default=str(ROOT / 'build/lab3c'))
    parser.add_argument('--vm', default=str(ROOT / 'tools/moar.sh'))
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--steps', type=int, default=1000)
    parser.add_argument('--spaces', type=int, nargs='+', default=[128, 256])
    parser.add_argument('--interval', type=int, default=100)
    parser.add_argument('--heap-words', type=int, default=4096)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/blackbox')
    parser.add_argument('--figures', type=Path, default=ROOT / 'output/figures/blackbox')
    parser.add_argument('--data-only', action='store_true')
    args = parser.parse_args()
    if (not 1 <= args.seed < 2147483647 or not 0 <= args.steps <= 10000 or args.interval < 1 or
            not 0 < args.timeout < float('inf') or not 0 < args.heap_words <= (1 << 31) - 1 or
            len(set(args.spaces)) != len(args.spaces)):
        parser.error('invalid seed, steps, interval, heap, timeout or duplicate spaces')
    roots = 2 * (max(args.spaces) // 6 + 1) + 1
    if any(s < 6 or 22 + roots + 2 * s > args.heap_words for s in args.spaces):
        parser.error('each semispace must hold an object; heap must fit state, root table and both semispaces')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(schema_version=3, demo=dict(id='blackbox', source='blackbox_gc.src', title='Случайный граф'),
                  measurement='active_semispace_occupancy_including_object_headers',
                  clock='MoarVM time: system epoch nanoseconds, relative to first sample',
                  config=dict(rounds=args.steps, seed=args.seed, interval=args.interval, roots=roots,
                              collection_policy='allocation_failure_only',
                              heap_words=args.heap_words, word_bytes=8, object_words=6,
                              probabilities=dict(allocate=.35, link=.30, release=.35),
                              workload=f'seed {args.seed}, {args.steps} операций'), runs=[])
    report['vm'] = subprocess.run([executable(args.vm), '--version'], check=True, capture_output=True,
                                  text=True, timeout=args.timeout).stdout.strip()
    for space in args.spaces:
        report['runs'].append(collect(args, space))
    data = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    (args.output / 'memory-profile.json').write_text(data)
    if not args.data_only:
        args.figures.mkdir(parents=True, exist_ok=True)
        plot_blackbox(report, args.figures)
        (args.figures / 'memory-profile.json').write_text(data)
        print(f'Graphs: {args.figures.resolve()}')
    for run in report['runs']:
        stats = run['stats']
        print(f'{run["semispace_words"] * 8} bytes: allocated {stats["allocated_objects"]}, '
              f'reclaimed {stats["reclaimed_objects"]}, collections {stats["collections"]} '
              f'({stats["automatic_collections"]} automatic), OOM {stats["allocation_failures"]}, '
              f'cyclic objects reclaimed {stats["cyclic_objects_reclaimed"]}, '
              f'checked snapshots {stats["verified_snapshots"]}, final {stats["final_used_words"] * 8} bytes, '
              f'reachable {stats["final_reachable_objects"]}, pending garbage {stats["pending_garbage_objects"]} objects')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        raise SystemExit(1)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)

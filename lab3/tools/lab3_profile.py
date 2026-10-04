import argparse
import json
from pathlib import Path
import subprocess
import sys

from lab3_demo import FIELDS, ROOT, executable

EVENTS = {0: 'init', 1: 'allocation', 2: 'before_gc', 3: 'after_gc', 4: 'finish'}
WORD_BYTES = 8
DEMOS = {
    'copying': dict(source='copying.src', title='Циклы и временные объекты',
                    spaces=[128, 32], min_space=19, min_roots=5, initial_objects=4),
    'animals': dict(source='animals_gc.src', title='Животные: собаки и кошки',
                    spaces=[64, 128], min_space=42, min_roots=8, initial_objects=7),
}


def parse_trace(stdout, space, rounds, demo='copying', require_empty=True):
    output, samples = [], []
    for line in stdout.splitlines():
        if not line.startswith('TRACE,'):
            output.append(line)
            continue
        fields = [int(x) for x in line.split(',')[1:]]
        if len(fields) != 9:
            raise ValueError('invalid profile record')
        elapsed, event, used, collections, allocated, survivors, copied, roots, edges = fields
        if event not in EVENTS or not 0 <= used <= space or elapsed < 0:
            raise ValueError('invalid trace event or occupancy')
        if samples and elapsed < samples[-1]['elapsed_ns']:
            raise ValueError('system clock moved backwards during profiling; repeat the run')
        samples.append(dict(elapsed_ns=elapsed, event=EVENTS[event], used_words=used,
                            used_bytes=used * WORD_BYTES, collections=collections,
                            allocated_objects=allocated, surviving_objects=survivors,
                            copied_objects=copied, updated_roots=roots, updated_fields=edges,
                            updated_links=roots + edges))
    if not samples or samples[0]['event'] != 'init' or samples[0]['elapsed_ns'] != 0:
        raise ValueError('incomplete profile')
    pending = None
    previous_used = allocations = collections = allocated_words = reclaimed_words = 0
    for index, sample in enumerate(samples):
        event, used = sample['event'], sample['used_words']
        if event == 'before_gc':
            if pending is not None or used != previous_used:
                raise ValueError('invalid start of collection')
            pending = sample
        elif event == 'after_gc':
            if pending is None or used > pending['used_words']:
                raise ValueError('invalid end of collection')
            reclaimed_words += pending['used_words'] - used
            pending = None
            collections += 1
        elif event == 'allocation':
            if pending is not None or used <= previous_used:
                raise ValueError('invalid allocation event')
            allocations += 1
            allocated_words += used - previous_used
        elif event == 'finish':
            if pending is not None or used != previous_used or index != len(samples) - 1:
                raise ValueError('invalid final observation')
        elif index != 0 or used != 0:
            raise ValueError('unexpected initialization')
        if sample['allocated_objects'] != allocations or sample['collections'] != collections:
            raise ValueError('trace counters do not agree with events')
        metrics = [sample[k] for k in ('surviving_objects', 'copied_objects', 'updated_roots', 'updated_fields')]
        if any(v < 0 for v in metrics):
            raise ValueError('negative collection metrics')
        if event == 'after_gc':
            survivors, copied, roots, edges = metrics
            if survivors != copied or survivors * 3 > used or (survivors == 0) != (used == 0):
                raise ValueError('surviving and copied object counts disagree')
            if roots + edges < survivors or edges > used - survivors * 3:
                raise ValueError('invalid updated reference counts')
        elif any(metrics):
            raise ValueError('collection metrics outside after_gc event')
        previous_used = used
    if pending is not None or (demo is not None and allocations != rounds + DEMOS[demo]['initial_objects']):
        raise ValueError('missing profile events')
    stats = dict(collections=collections, allocated_objects=allocations,
                 allocated_words=allocated_words, reclaimed_words=reclaimed_words,
                 peak_words=max(s['used_words'] for s in samples), final_used_words=previous_used)
    if allocated_words != reclaimed_words + previous_used:
        raise ValueError('allocated memory does not equal reclaimed plus occupied memory')
    if require_empty and (samples[-1]['event'] != 'after_gc' or previous_used != 0):
        raise ValueError('final collection did not reclaim all objects')
    after = [sample for sample in samples if sample['event'] == 'after_gc']
    if demo == 'copying':
        values = [int(line) for line in output]
        if len(values) != len(FIELDS):
            raise ValueError('incomplete copying demo output')
        declared = dict(zip(FIELDS, values))
        if any(declared[key] != value for key, value in stats.items()) or declared['checksum'] != 579:
            raise ValueError('profile does not agree with copying demo statistics')
        stats.update(declared)
    elif demo == 'animals':
        if not output or output[-1] != 'ANIMALS GC: OK' or collections < 4:
            raise ValueError('incomplete animals demo output')
        if any(sample['used_words'] != 18 for sample in after[:-1]):
            raise ValueError('the three cats were not preserved')
        before = [sample for sample in samples if sample['event'] == 'before_gc']
        if before[0]['used_words'] != 42 or before[0]['allocated_objects'] != 7:
            raise ValueError('invalid initial animal population')
        stats['automatic_collections'] = collections - 4
        if f'Автоматических сборок: {stats["automatic_collections"]}' not in output:
            raise ValueError('profile does not agree with animals demo statistics')
    return stats, samples


def collect(args, space):
    demo = DEMOS[args.demo]
    directory = args.output / f'space{space}'
    directory.mkdir(parents=True, exist_ok=True)
    bytecode = directory / (Path(demo['source']).stem + '.moarvm')
    capacity = 3 * (args.rounds + demo['initial_objects']) + 9
    command = [executable(args.compiler), '--heap-words', str(args.heap_words),
               '--profile-capacity', str(capacity)]
    for value in (space, args.roots, args.rounds):
        command += ['--arg', str(value)]
    command += [str(bytecode), str(ROOT / 'runtime/gc.src'), str(ROOT / 'examples' / demo['source'])]
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=args.timeout)
    vm = executable(args.vm)
    result = subprocess.run([vm, str(bytecode)], check=True, capture_output=True,
                            text=True, timeout=args.timeout)
    stats, samples = parse_trace(result.stdout, space, args.rounds, args.demo)
    (directory / 'stdout.txt').write_text(result.stdout)
    return dict(semispace_words=space, stats=stats, samples=samples,
                trace_buffer_bytes=capacity * 9 * WORD_BYTES)


def plot(report, output):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MaxNLocator
    except ImportError as error:
        raise RuntimeError('Install plotting dependencies: make setup-plots') from error
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 11,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'axes.titleweight': 'bold', 'savefig.facecolor': 'white',
                         'svg.fonttype': 'none'})
    plot_collection_metrics(report, output)
    runs = report['runs']
    colors = ['#2563a6', '#087f8c', '#7356a6', '#b56226']
    fig, axes = plt.subplots(len(runs), 1, figsize=(12, 3.7 * len(runs)), squeeze=False)
    for index, (ax, run) in enumerate(zip(axes[:, 0], runs)):
        samples, space, stats = run['samples'], run['semispace_words'], run['stats']
        x = [s['elapsed_ns'] / 1e6 for s in samples]
        y = [s['used_bytes'] for s in samples]
        color = colors[index % len(colors)]
        ax.step(x, y, where='post', color=color, linewidth=1.6, label='Занято в активном полупространстве')
        ax.fill_between(x, y, step='post', color=color, alpha=.10)
        after = [s for s in samples if s['event'] == 'after_gc']
        ax.scatter([s['elapsed_ns'] / 1e6 for s in after], [s['used_bytes'] for s in after],
                   color='#c4562e', marker='v', s=28, zorder=4, label='Завершение сборки мусора')
        ax.axhline(space * WORD_BYTES, color='#8c96a2', ls='--', lw=1, label='Ёмкость полупространства')
        ax.set(title=f'Полупространство {space * WORD_BYTES} байт ({space} слов)  ·  {stats["collections"]} сборок  ·  '
                     f'пик {stats["peak_words"] * WORD_BYTES} байт',
               xlabel='Время с инициализации кучи, мс', ylabel='Занято, байт',
               ylim=(0, space * WORD_BYTES * 1.15), xlim=(0, max(x) * 1.025))
        ax.yaxis.set_major_locator(MaxNLocator(integer=True, nbins=5))
        ax.grid(axis='y', color='#e4e8ed')
    fig.suptitle('Профиль использованной памяти во времени', fontsize=19, fontweight='bold', y=.985)
    fig.legend(*axes[0, 0].get_legend_handles_labels(), loc='upper center',
               bbox_to_anchor=(.52, .905), ncol=3, fontsize=9, frameon=False)
    workload = report['config'].get('workload', f'{report["config"]["rounds"]} временных объектов')
    fig.text(.5, .935, f'{report["demo"]["source"]} · {report["demo"]["title"]} · {workload}',
             ha='center', color='#536170', fontsize=11)
    fig.text(.07, .017, 'Измерена занятость объектов вместе с заголовками. Буфер трассы и память процесса MoarVM не включены.\n'
             'Временные метки сняты внутри ВМ; запись трассы влияет на длительность опыта.', fontsize=9, color='#536170')
    fig.subplots_adjust(top=.81, bottom=.14, hspace=.62, left=.085, right=.975)
    for extension in ('png', 'svg'):
        fig.savefig(output / f'memory-profile-time.{extension}', dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(12, 5))
    for index, run in enumerate(runs):
        samples = run['samples']
        ax.plot([s['allocated_objects'] for s in samples], [s['used_bytes'] for s in samples],
                drawstyle='steps-post', color=colors[index % len(colors)], lw=1.6,
                label=f'Полупространство {run["semispace_words"] * WORD_BYTES} байт ({run["semispace_words"]} слов)')
    ax.set(xlabel='Количество завершённых выделений (логическое время)', ylabel='Занято, байт',
           title='', ylim=(0, None))
    fig.suptitle(f'{report["demo"]["source"]}: сравнение по числу выделений',
                 y=.96, fontsize=15, fontweight='bold')
    ax.grid(axis='y', color='#e4e8ed')
    ax.legend(loc='lower left', bbox_to_anchor=(0, 1.015), ncol=2, fontsize=10, frameon=False)
    ending = ('В конце сняты корни; оставшийся мусор ожидает следующей сборки.'
              if report['demo']['id'] == 'blackbox' else 'Последняя сборка освобождает все объекты.')
    fig.text(.085, .035, 'Вертикальные падения — сборки мусора. ' + ending,
             fontsize=10, color='#536170')
    fig.subplots_adjust(bottom=.2, top=.79, left=.085, right=.975)
    for extension in ('png', 'svg'):
        fig.savefig(output / f'memory-profile-allocations.{extension}', dpi=180)
    plt.close(fig)

    if report['demo']['id'] == 'animals':
        fig, ax = plt.subplots(figsize=(12, 5.5))
        for index, run in enumerate(runs):
            before = [s for s in run['samples'] if s['event'] == 'before_gc']
            after = [s for s in run['samples'] if s['event'] == 'after_gc']
            values = [before[0]['used_bytes'], after[0]['used_bytes'],
                      after[-3]['used_bytes'], after[-2]['used_bytes'], after[-1]['used_bytes']]
            ax.plot(range(5), values, marker='o', markersize=7, linewidth=2,
                    linestyle='-' if index == 0 else '--', color=colors[index % len(colors)],
                    label=f'{run["semispace_words"] * WORD_BYTES} байт: те же живые объекты')
        for x, y in enumerate(values):
            ax.annotate(f'{y} байт', (x, y), xytext=(0, 13), textcoords='offset points',
                        ha='center', fontsize=11, fontweight='bold')
        ax.set_xticks(range(5), ['7 животных', 'GC после отбора\n3 кошек',
                               'GC после временных\nживотных', 'GC: остался\nтолько alias',
                               'GC: снят\nпоследний корень'])
        ax.set(ylabel='Занято, байт', ylim=(0, 400), xlim=(-.35, 4.35))
        ax.grid(axis='y', color='#e4e8ed')
        ax.legend(loc='upper right', frameon=False, fontsize=9)
        fig.suptitle('animals_gc.src: что сохраняется после сборки', fontsize=17, fontweight='bold')
        fig.text(.085, .04, 'Точки взяты из трасс измеренных запусков. Это этапы программы, а не равные интервалы времени.',
                 fontsize=10, color='#536170')
        fig.subplots_adjust(bottom=.22, top=.86, left=.085, right=.975)
        for extension in ('png', 'svg'):
            fig.savefig(output / f'memory-profile-stages.{extension}', dpi=180)
        plt.close(fig)


def plot_collection_metrics(report, output):
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator
    for run in report['runs']:
        samples = [s for s in run['samples'] if s['event'] == 'after_gc']
        indices = list(range(1, len(samples) + 1))
        width = .24
        fig, ax = plt.subplots(figsize=(max(10, min(18, len(samples) * .45)), 5.8))
        survivors = [s['surviving_objects'] for s in samples]
        copied = [s['copied_objects'] for s in samples]
        roots = [s['updated_roots'] for s in samples]
        fields = [s['updated_fields'] for s in samples]
        if len(samples) > 60:
            ax.plot(indices, [a + b for a, b in zip(roots, fields)], color='#dd8520', linewidth=1.4,
                    label='Обновлено ссылок всего')
            ax.plot(indices, fields, color='#7960a5', linewidth=1, alpha=.8,
                    label='Из них внутри объектов')
            ax.plot(indices, survivors, color='#188650', linewidth=2, label='Выжило объектов')
            ax.plot(indices, copied, color='#2466ad', linestyle='--', linewidth=1,
                    label='Скопировано объектов')
        else:
            live_bars = ax.bar([i - width for i in indices], survivors, width, color='#188650', label='Выжило объектов')
            copy_bars = ax.bar(indices, copied, width, color='#2466ad', label='Скопировано объектов')
            link_bars = ax.bar([i + width for i in indices], roots, width, color='#dd8520', label='Обновлено ссылок в корнях')
            ax.bar([i + width for i in indices], fields, width, bottom=roots, color='#7960a5', label='Обновлено ссылок в объектах')
            if len(samples) <= 16:
                ax.bar_label(live_bars, padding=3, fontsize=9)
                ax.bar_label(copy_bars, padding=3, fontsize=9)
                for bar, a, b in zip(link_bars, roots, fields):
                    ax.annotate(str(a + b), (bar.get_x() + width / 2, a + b),
                                xytext=(0, 3), textcoords='offset points', ha='center', fontsize=9)
        ticks = indices if len(indices) <= 20 else sorted(set(indices[::max(1, len(indices) // 12)] + indices[-1:]))
        if len(ticks) > 2 and ticks[-1] - ticks[-2] < (ticks[1] - ticks[0]) / 2:
            ticks.pop(-2)
        ax.set_xticks(ticks)
        peak = max(survivors + copied + [a + b for a, b in zip(roots, fields)] + [0])
        ax.set(xlabel='Номер сборки мусора', ylabel='Количество за одну сборку',
               ylim=(0, max(2, peak * 1.25)))
        if peak == 0:
            message = 'На момент этих сборок достижимых объектов не осталось' if samples else 'Сборки не потребовались: выделения поместились в полупространстве'
            ax.text(.5, .5, message,
                    transform=ax.transAxes, ha='center', color='#536170', fontsize=11)
        ax.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax.margins(y=.22)
        ax.grid(axis='y', alpha=.2)
        ax.set_axisbelow(True)
        ax.legend(loc='upper left', bbox_to_anchor=(0, 1.19), ncol=2, fontsize=10, frameon=False)
        fig.suptitle(f'{report["demo"]["source"]}: результат каждой сборки · {run["semispace_words"] * 8} байт',
                     fontsize=15, fontweight='bold')
        explanation = ('Показаны все сборки без усреднения. Линии выживших и скопированных совпадают.'
                       if len(samples) > 60 else 'Третий столбец — сумма ссылок в корнях и объектах.')
        fig.text(.085, .035, 'Чейни копирует каждый выживший объект ровно один раз. Ссылки считаются по изменённым адресам.\n'
                 + explanation + ' Пустые ссылки и forwarding-заголовки не включены.',
                 fontsize=9, color='#536170')
        fig.subplots_adjust(top=.75, bottom=.20, left=.085, right=.98)
        for extension in ('png', 'svg'):
            fig.savefig(output / f'gc-collections-space{run["semispace_words"]}.{extension}', dpi=180)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description='Record MoarVM GC events and plot measured heap occupancy against elapsed time.')
    parser.add_argument('--compiler', default=str(ROOT / 'build/lab3c'))
    parser.add_argument('--vm', default=str(ROOT / 'tools/moar.sh'))
    parser.add_argument('--demo', choices=DEMOS, default='copying')
    parser.add_argument('--spaces', nargs='+', type=int)
    parser.add_argument('--rounds', type=int, default=200)
    parser.add_argument('--roots', type=int, default=16)
    parser.add_argument('--heap-words', type=int, default=4096)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--output', type=Path, default=None)
    parser.add_argument('--figures', type=Path, default=None)
    parser.add_argument('--data-only', action='store_true')
    args = parser.parse_args()
    demo = DEMOS[args.demo]
    args.spaces = args.spaces or demo['spaces']
    args.output = args.output or ROOT / 'output/profile' / args.demo
    args.figures = args.figures or ROOT / 'output/figures' / args.demo
    if (not 0 <= args.rounds <= 1_000_000 or args.roots < demo['min_roots'] or
            not 0 < args.heap_words <= (1 << 31) - 1 or
            not 0 < args.timeout < float('inf') or len(set(args.spaces)) != len(args.spaces)):
        parser.error('invalid configuration: rounds 0..1000000, enough roots for the demo, positive heap/timeout, distinct spaces')
    for space in args.spaces:
        if space < demo['min_space'] or 1 + 21 + args.roots + 2 * space > args.heap_words:
            parser.error(f'semispace {space} is too small for the demo or exceeds the heap')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(schema_version=3, demo=dict(id=args.demo, source=demo['source'], title=demo['title']),
                  measurement='active_semispace_occupancy_including_object_headers',
                  clock='MoarVM time: system epoch nanoseconds, relative to first sample',
                  config=dict(rounds=args.rounds, roots=args.roots, heap_words=args.heap_words,
                              word_bytes=WORD_BYTES), runs=[])
    report['vm'] = subprocess.run([executable(args.vm), '--version'], check=True, text=True,
                                  capture_output=True, timeout=args.timeout).stdout.strip()
    for space in args.spaces:
        report['runs'].append(collect(args, space))
    data = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    (args.output / 'memory-profile.json').write_text(data)
    if not args.data_only:
        args.figures.mkdir(parents=True, exist_ok=True)
        plot(report, args.figures)
        (args.figures / 'memory-profile.json').write_text(data)
        print(f'Graphs and measured data: {args.figures.resolve()}')
    for run in report['runs']:
        print(f'{run["semispace_words"]} words: {len(run["samples"])} samples, '
              f'{run["stats"]["collections"]} collections, '
              f'{run["samples"][-1]["elapsed_ns"] / 1e6:.3f} ms, final occupancy 0 bytes')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        raise SystemExit(1)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)

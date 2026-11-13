#!/usr/bin/env python3
"""Group machoman audit TSV reports into a Markdown compatibility inventory."""
import argparse
import collections
import csv
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('reports', nargs='+', type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
groups = collections.defaultdict(lambda: collections.defaultdict(set))
lines = ['# Import compatibility inventory', '',
         'Generated from `machoman audit`. Guest initializers and main were not executed.', '',
         '`missing` means the loader found no definition. `resolver` means a framework generated',
         'an address; it may represent a function stub, Objective-C metadata, or a constant.',
         'Host and guest resolutions still require ABI/behavior checks. Dynamic Objective-C',
         'selector calls and libraries opened later with dlopen are outside this inventory.', '',
         '| Report | Bind slots | Missing | Resolver | Weak null |',
         '| --- | ---: | ---: | ---: | ---: |']

def cell(text):
    return text.replace('|', '\\|').replace('\n', ' ')

for report in args.reports:
    with report.open() as source:
        rows = list(csv.DictReader(source, delimiter='\t'))
    counts = collections.Counter(row['resolution'] for row in rows)
    binds = sum(value for key, value in counts.items() if key not in ('missing-dylib', 'weak-dylib'))
    lines.append(f'| {cell(report.name)} | {binds} | {counts["missing"]} | {counts["resolver"]} | {counts["weak-null"]} |')
    for row in rows:
        if row['resolution'] in ('missing', 'resolver', 'missing-dylib', 'weak-dylib', 'weak-null'):
            groups[row['resolution']][row['dependency'], row['symbol']].add(Path(row['image']).name)

lines += ['', '## Required unresolved symbols', '',
          'Some entries, such as `dyld_stub_binder`, are unused by the eager binder.',
          'The inventory records resolution; it does not establish which paths startup executes.', '']
for dependency in sorted({key[0] for key in groups['missing']}):
    lines += [f'### {dependency}', '', '| Symbol | Importing images |', '| --- | --- |']
    for (dep, symbol), images in sorted(groups['missing'].items()):
        if dep == dependency:
            lines.append(f'| `{cell(symbol)}` | {cell(", ".join(sorted(images)))} |')
    lines.append('')

for status, title in [('missing-dylib', 'Missing required libraries'),
                      ('weak-dylib', 'Missing optional libraries'), ('weak-null', 'Optional unresolved symbols'),
                      ('resolver', 'Framework resolver results to review')]:
    lines += [f'## {title}', '']
    if not groups[status]:
        lines += ['None.', '']
        continue
    for dependency in sorted({key[0] for key in groups[status]}):
        entries = [(symbol, images) for (dep, symbol), images in sorted(groups[status].items()) if dep == dependency]
        lines += [f'### {dependency} ({len(entries)})', '']
        for symbol, images in entries:
            label = f'`{cell(symbol)}`' if symbol else cell(', '.join(sorted(images)))
            lines.append(f'- {label}')
        lines.append('')
args.output.write_text('\n'.join(lines))
print(f'{len(groups["missing"])} unique required missing symbols; {len(groups["resolver"])} unique resolver results; {args.output}')

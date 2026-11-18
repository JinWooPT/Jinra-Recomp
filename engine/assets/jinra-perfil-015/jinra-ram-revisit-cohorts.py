"""Plan a native, paced retirement of the previous zone's known resources."""

import json
from pathlib import Path
import runpy
import sys


def prepare_revisit(folder, source, destination, tag):
    helpers = runpy.run_path(str(Path(__file__).resolve().parent / 'release-batch.py'))
    catalogue = json.loads((Path(__file__).resolve().parent / 'catalogue-across-orders-015.json').read_text())['by_zone']
    protected = {
        identity for path in Path(__file__).resolve().parent.glob('protected-identities-trace-*.json')
        for identity in json.loads(path.read_text())
    }
    marks = {
        row['label']: row
        for row in map(json.loads, (folder / 'marks.jsonl').read_text().splitlines())
    }
    previous = marks[source + '_census_end']
    zone = previous['map']
    if zone == marks[destination + '_play']['map']:
        raise ValueError('Retirement source must be the zone just exited')
    cohort = [row for row in catalogue.get(zone, ()) if row['resource_id'] not in protected]
    candidates = [
        (size, node, base, previous['qpc_ns'], zone)
        for node, base, size in helpers['available'](folder, cohort, source, destination)
    ]
    if candidates:
        helpers['write_candidates'](folder, candidates, tag)
    else:
        (folder / f'{tag}-result.json').write_text('[]\n', encoding='utf-8')
    return bool(candidates)



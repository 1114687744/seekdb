#!/usr/bin/env python3
"""Check stream freshness and recall against a matching baseline run."""
import argparse
import json
from pathlib import Path
from analyze import summarize


def compare(baseline, candidate, freshness_slack_ms, recall_slack):
    metadata = [json.loads((p / 'metadata.json').read_text())
                for p in (baseline, candidate)]
    for key in ('rows', 'queries', 'rate', 'batch', 'readers', 'probe_interval',
                'drain_seconds', 'settle_seconds', 'server_affinity'):
        if metadata[0].get(key, .5 if key == 'probe_interval' else None) != metadata[1].get(
                key, .5 if key == 'probe_interval' else None):
            raise ValueError('mismatched measurement setting: ' + key)
    base, new = (summarize(p) for p in (baseline, candidate))
    checks = []

    def check(name, actual, limit, passed):
        checks.append(dict(check=name, actual=actual, limit=limit, passed=bool(passed)))

    for label, result in (('baseline', base), ('candidate', new)):
        check(label + '_errors', result['errors'], [], not result['errors'])
        check(label + '_committed', result['committed'], metadata[0]['rows'],
              result['committed'] == metadata[0]['rows'])
        check(label + '_censored_probes', result['freshness_censored_positions'], [],
              not result['freshness_censored_positions'])
    actual = new['freshness_upper_bound_ms']['p95']
    limit = base['freshness_upper_bound_ms']['p95'] + freshness_slack_ms
    check('freshness_p95_ms', actual, limit, actual <= limit)
    for name, actual, reference in (
            ('recall_20k_to_80k', new['query_window_20k_to_80k']['stable_recall'],
             base['query_window_20k_to_80k']['stable_recall']),
            ('final_recall', new['final_recall'], base['final_recall'])):
        limit = reference - recall_slack
        check(name, actual, limit, actual is not None and actual >= limit)
    pending = [count for name, count in new['final_auxiliary_rows'].items()
               if name.endswith('_idx')]
    check('candidate_pending_delta', pending, [0], pending == [0])
    return dict(baseline=baseline.name, candidate=candidate.name, checks=checks,
                passed=all(c['passed'] for c in checks))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--freshness-slack-ms', type=float, default=100)
    parser.add_argument('--recall-slack', type=float, default=.01)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = compare(args.baseline, args.candidate, args.freshness_slack_ms, args.recall_slack)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
    print(output, end='')
    raise SystemExit(0 if result['passed'] else 1)

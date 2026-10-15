#!/usr/bin/env python3
"""Summarize saved stream runs, including a matched committed-prefix window."""
import argparse
import json
from pathlib import Path
import numpy as np
from stream_bench import percentiles


def lines(path):
    return [json.loads(line) for line in path.open()]


def summarize(path):
    summary = json.loads((path/'summary.json').read_text())
    meta = json.loads((path/'metadata.json').read_text())
    writes, queries = lines(path/'writes.jsonl'), lines(path/'queries.jsonl')
    resources, backlog, probes = lines(path/'resources.jsonl'), lines(path/'backlog.jsonl'), lines(path/'freshness.jsonl')
    t0 = next(w['commit_s'] for w in writes if w['hi'] >= 20000)
    t1 = next(w['commit_s'] for w in writes if w['hi'] >= 80000)
    window = [r for r in queries if t0 <= r['start_s'] and r['end_s'] <= t1]
    stable = [r for r in window if r['prefix_start'] == r['prefix_end']]
    cpu = [r for r in resources if r['time_s'] <= summary['write_phase_s']]
    drain_tail = [r for r in resources if r['time_s'] >= summary['write_phase_s']+20]
    def cores(samples):
        if len(samples)<2: return None
        first,last=samples[0],samples[-1]
        return sum(last['cpu'][k]-first['cpu'][k] for k in ('user','system'))/(last['time_s']-first['time_s'])
    first_visible = {}
    last_negative = {}
    for r in probes:
        if not r['visible']:
            last_negative[r['position']] = max(0,r['probe_s']-r['commit_s'])*1000
        elif r['position'] not in first_visible:
            first_visible[r['position']] = max(0,r['probe_s']-r['commit_s'])*1000
    result = {k:v for k,v in summary.items() if k not in ('final',)}
    started_during_write = [r for r in queries if r["start_s"] < summary["write_phase_s"]]
    result.update(search_started_during_write_latency_ms=percentiles([r["latency_ms"] for r in started_during_write]),
        run=path.name, offered_rate=meta['rate'], mode=meta['mode'],
        final_rows=summary['final']['rows'][0][0],
        final_recall=float(np.mean(summary['final']['recall'])),
        query_window_20k_to_80k=dict(seconds=t1-t0, qps=len(window)/(t1-t0),
            latency_ms=percentiles([r['latency_ms'] for r in window]),
            stable_recall=float(np.mean([r['recall_at_start'] for r in stable])) if stable else None,
            samples=len(window)),
        average_cpu_cores_during_write=cores(cpu), average_cpu_cores_drain_tail=cores(drain_tail),
        peak_rss_mib=max(r['rss'] for r in resources)/(1024*1024),
        process_write_mib=(resources[-1]['io']['write_bytes']-resources[0]['io']['write_bytes'])/(1024*1024),
        process_read_mib=(resources[-1]['io']['read_bytes']-resources[0]['io']['read_bytes'])/(1024*1024),
        last_auxiliary_observation=backlog[-1],
        freshness_last_negative_ms=percentiles(list(last_negative.values())),
        server_affinity=meta['server_affinity'], server_exe=meta['server_exe'])
    result['final_auxiliary_rows'] = summary['final'].get('auxiliary_rows')
    result['background_worker_counts'] = {
        prefix: {
            'peak_observed': max(sum(t['name'].startswith(prefix) for t in r.get('background_workers', []))
                                 for r in resources),
            'last_observed': sum(t['name'].startswith(prefix)
                                 for t in resources[-1].get('background_workers', []))}
        for prefix in ('VecBuild', 'VecDelta', 'CSWorker')} if 'background_workers' in resources[0] else None
    empty = [r for r in backlog if r['time_s'] >= summary['write_phase_s']
             and any(k.endswith('_idx') for k in r['rows'])
             and all(v == 0 for k,v in r['rows'].items() if k.endswith('_idx'))]
    result['delta_empty_observed_after_write_s'] = (
        empty[0]['time_s'] - summary['write_phase_s'] if empty else None)
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('runs',nargs='+')
    p.add_argument('--output',required=True)
    a=p.parse_args()
    result=[summarize(Path(path)) for path in a.runs]
    Path(a.output).write_text(json.dumps(result,indent=2)+'\n')
    for r in result:
        print(r['run'], 'rows/s',round(r['actual_write_rows_s']), 'write P99',round(r['write_latency_ms']['p99'],2),
              'search P99',round(r['search_started_during_write_latency_ms']['p99'],2), 'QPS',round(r['search_qps_during_write']),
              'recall',round(r['final_recall'],4), 'CPU',round(r['average_cpu_cores_during_write'],2),
              'RSS MiB',round(r['peak_rss_mib']))

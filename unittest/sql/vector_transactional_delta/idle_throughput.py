#!/usr/bin/env python3
"""Measure one unpaced writer and true idle CPU on a fresh dedicated instance."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
from types import SimpleNamespace
import numpy as np
import psutil
from stream_bench import connect, percentiles, prepare, sql, vector


def snapshot(process):
    threads = {}
    for thread in process.threads():
        try:
            base = Path(f'/proc/{process.pid}/task/{thread.id}')
            name = (base/'comm').read_text().strip()
            status = dict(line.split(':', 1) for line in (base/'status').read_text().splitlines())
            threads[str(thread.id)] = dict(name=name, cpu_s=thread.user_time+thread.system_time,
                voluntary=int(status['voluntary_ctxt_switches']),
                involuntary=int(status['nonvoluntary_ctxt_switches']))
        except (FileNotFoundError, ProcessLookupError):
            pass
    cpu = process.cpu_times()
    return dict(time_s=time.monotonic(), cpu_s=cpu.user+cpu.system,
                rss=process.memory_info().rss, threads=threads, host_load=os.getloadavg())


def idle(process, seconds, path):
    # No SQL, ANN queries, or database connection is used inside this interval.
    samples = [snapshot(process)]
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        time.sleep(min(1, max(0, deadline-time.monotonic())))
        samples.append(snapshot(process))
    first, last = samples[0], samples[-1]
    elapsed = last['time_s']-first['time_s']
    activity = []
    for tid, end in last['threads'].items():
        begin = first['threads'].get(tid)
        if begin is not None and begin['name'] == end['name']:
            activity.append(dict(tid=tid, name=end['name'],
                cpu_percent_one_core=100*(end['cpu_s']-begin['cpu_s'])/elapsed,
                voluntary_switches=end['voluntary']-begin['voluntary'],
                involuntary_switches=end['involuntary']-begin['involuntary']))
    activity.sort(key=lambda item: item['cpu_percent_one_core'], reverse=True)
    path.write_text(json.dumps(samples, indent=2)+'\n')
    return dict(seconds=elapsed, cpu_percent_one_core=100*(last['cpu_s']-first['cpu_s'])/elapsed,
                rss_mib=last['rss']/1024**2, threads=activity,
                final_thread_count=len(last['threads']),
                build_workers=sum(t['name'].startswith(('VecBuild','CSWorker')) for t in last['threads'].values()))


def measure(args):
    root = Path(args.root).absolute()
    assert str(root).startswith('/data/1/nijia.nj/test/vector-delta-')
    assert args.label.isalnum()
    base = root/args.label
    out = root/'results'/args.label
    assert not base.exists() and not out.exists()
    out.mkdir()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', args.port))
    command = ['taskset','-c','48-55',str(root/'bin'/args.binary),'--base-dir='+str(base),
               '-P'+str(args.port),'--log-level=WARN','--parameter','memory_budget=6G',
               '--parameter','cpu_count=8','--parameter','datafile_size=2G',
               '--parameter','datafile_maxsize=8G','--parameter','log_disk_size=4G',
               '--parameter','internal_sql_execute_timeout=120s']
    with (out/'startup.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    process = None
    report = dict(arguments=vars(args), server_command=command)

    def save(stage):
        (out/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(dict(run=args.label, stage=stage,
            result={k:v for k,v in report[stage].items() if k != 'threads'})), flush=True)

    try:
        deadline = time.monotonic()+120
        while True:
            try:
                c=connect(args); c.close(); break
            except Exception:
                if time.monotonic()>deadline: raise
                time.sleep(.5)
        process = psutil.Process(int((base/'run/seekdb.pid').read_text()))
        report['pid'] = process.pid
        report['affinity'] = process.cpu_affinity()
        time.sleep(15)
        report['no_index_idle'] = idle(process, args.idle_seconds, out/'no_index_idle.json')
        save('no_index_idle')

        database = 'vecdelta_'+args.label
        metadata = prepare(SimpleNamespace(port=args.port, database=database, mode='async'))
        report['metadata'] = metadata
        time.sleep(15)
        report['empty_index_idle'] = idle(process, args.idle_seconds, out/'empty_index_idle.json')
        save('empty_index_idle')

        vectors = np.load(root/'data/base.npy', mmap_mode='r')[:args.rows]
        ids = np.load(root/'data/ids.npy', mmap_mode='r')[:args.rows]
        c = connect(args)
        sql(c, 'USE `'+database+'`')
        writes=[]
        client_start=time.process_time()
        start=time.monotonic()
        for lo in range(0,args.rows,args.batch):
            hi=min(lo+args.batch,args.rows)
            prep=time.monotonic()
            values=','.join(f'({int(ids[i])},{vector(vectors[i])})' for i in range(lo,hi))
            sent=time.monotonic()
            sql(c,'INSERT INTO docs VALUES '+values)
            done=time.monotonic()
            writes.append(dict(lo=lo,hi=hi,prepare_ms=(sent-prep)*1000,
                sql_ms=(done-sent)*1000,commit_s=done-start))
        write_end=time.monotonic()
        report['write'] = dict(rows=args.rows,seconds=write_end-start,
            client_committed_rows_s=args.rows/(write_end-start),batch=args.batch,
            client_cpu_s=time.process_time()-client_start,
            batch_sql_ms=percentiles([w['sql_ms'] for w in writes]),
            prepare_total_s=sum(w['prepare_ms'] for w in writes)/1000,
            note='One synchronous client, no pacing and no concurrent ANN readers; includes client formatting.')
        (out/'writes.json').write_text(json.dumps(writes,indent=2)+'\n')
        save('write')
        assert sql(c,'SELECT COUNT(*) FROM docs')[0][0] == args.rows
        sql(c,'CALL dbms_index_manager.refresh()')
        barrier_end=time.monotonic()
        sql(c,'SET ob_enable_index_direct_select=1')
        counts={name:sql(c,f'SELECT COUNT(*) FROM `{name}`')[0][0]
                for (name,) in metadata['auxiliary_tables']}
        assert all(count == 0 for name,count in counts.items() if name.endswith('_idx')), counts
        assert sum(count for name,count in counts.items() if name.endswith('_index_id_table')) == args.rows, counts
        report['caught_up'] = dict(barrier_after_write_s=barrier_end-write_end,
            write_and_barrier_s=barrier_end-start,rows_s_through_barrier=args.rows/(barrier_end-start),
            auxiliary_rows=counts)
        c.close()
        save('caught_up')
        time.sleep(15)
        report['loaded_index_idle'] = idle(process,args.idle_seconds,out/'loaded_index_idle.json')
        save('loaded_index_idle')
        if args.profile_loaded:
            tids=[t.id for t in process.threads()
                  if Path(f'/proc/{process.pid}/task/{t.id}/comm').read_text().strip() == 'VecDelta']
            assert len(tids) == 1, tids
            # Profile after the timed idle interval, so profiler overhead is
            # excluded from the reported CPU measurement.
            with (out/'perf.log').open('w') as log:
                result=subprocess.run(['perf','record','-e','cycles:u','-F','199',
                    '-g','--call-graph','dwarf,8192','-t',str(tids[0]),
                    '-o',str(out/'perf.data'),'--','sleep','15'],stdout=log,stderr=subprocess.STDOUT)
            report['profile'] = dict(exit_code=result.returncode, tid=tids[0])
            if result.returncode == 0:
                with (out/'perf-report.txt').open('w') as log:
                    subprocess.run(['perf','report','--stdio','--no-children','--percent-limit','1',
                        '-i',str(out/'perf.data')],stdout=log,stderr=subprocess.STDOUT,check=True)
            save('profile')
    finally:
        if process is None and (base/'run/seekdb.pid').exists():
            process=psutil.Process(int((base/'run/seekdb.pid').read_text()))
        if process is not None and process.is_running():
            assert str(base).encode() in Path(f'/proc/{process.pid}/cmdline').read_bytes()
            os.kill(process.pid,signal.SIGTERM)
            deadline=time.monotonic()+60
            while Path(f'/proc/{process.pid}').exists():
                if time.monotonic()>deadline: raise TimeoutError('stop')
                time.sleep(.2)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',required=True)
    parser.add_argument('--label',required=True)
    parser.add_argument('--binary',required=True)
    parser.add_argument('--port',type=int,default=15881)
    parser.add_argument('--rows',type=int,default=100000)
    parser.add_argument('--batch',type=int,default=100)
    parser.add_argument('--idle-seconds',type=int,default=30)
    parser.add_argument('--profile-loaded',action='store_true')
    args=parser.parse_args()
    assert min(args.rows,args.batch,args.idle_seconds)>0
    measure(args)

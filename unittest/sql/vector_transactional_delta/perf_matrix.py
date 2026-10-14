#!/usr/bin/env python3
"""Run the three implementations sequentially, each on a fresh dedicated instance."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from types import SimpleNamespace
from stream_bench import connect


def run(args):
    root = Path(args.root).absolute()
    assert str(root).startswith('/data/1/nijia.nj/test/vector-delta-')
    matrix = [('async',1000),('delta',1000),('immediate',1000),
              ('immediate',4000),('delta',4000),('async',4000)]
    failures = []
    for impl,rate in matrix:
        name = f'{args.label}_{impl}_100k_{rate}'
        base = root / name
        assert not base.exists(), str(base)
        binary = root / 'bin' / ('seekdb-delta-perf' if impl=='delta' else 'seekdb-master-perf')
        with (root/'results'/f'{name}.start.log').open('w') as log:
            subprocess.run(['taskset','-c','48-55',str(binary),'--base-dir='+str(base),
                            '-P15881','--log-level=WARN','--parameter','memory_budget=6G',
                            '--parameter','cpu_count=8','--parameter','datafile_size=2G',
                            '--parameter','datafile_maxsize=8G','--parameter','log_disk_size=4G',
                            '--parameter','max_syslog_file_count=8'],stdout=log,stderr=subprocess.STDOUT,check=True)
        pid = None
        try:
            deadline = time.monotonic()+120
            while True:
                try:
                    c = connect(SimpleNamespace(port=15881)); c.close(); break
                except Exception:
                    if time.monotonic()>deadline: raise
                    time.sleep(.5)
            pid = int((base/'run/seekdb.pid').read_text())
            print(json.dumps(dict(start=name,pid=pid,time=time.time())),flush=True)
            command=['taskset','-c','56-59','python3.8',str(root/'scripts/stream_bench.py'),
                     '--port','15881','--pid',str(pid),'--database','vecdelta_'+name,
                     '--mode','immediate' if impl=='immediate' else 'async','--rows','100000',
                     '--queries','100','--rate',str(rate),'--settle-seconds','12',
                     '--data',str(root/'data'),'--output',str(root/'results'/name)]
            with (root/'results'/f'{name}.log').open('w') as log:
                result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
            print(json.dumps(dict(finish=name,exit_code=result.returncode,time=time.time())),flush=True)
            if result.returncode:
                failures.append(name)
        finally:
            if pid is None and (base/'run/seekdb.pid').exists():
                pid=int((base/'run/seekdb.pid').read_text())
            if pid and Path(f'/proc/{pid}').exists():
                assert str(base).encode() in Path(f'/proc/{pid}/cmdline').read_bytes()
                os.kill(pid,signal.SIGTERM)
                deadline=time.monotonic()+60
                while Path(f'/proc/{pid}').exists():
                    if time.monotonic()>deadline: raise TimeoutError('stop '+str(pid))
                    time.sleep(.2)
    print(json.dumps(dict(complete=True, failures=failures)),flush=True)
    if failures:
        raise SystemExit(1)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',required=True)
    p.add_argument('--label',required=True)
    args=p.parse_args()
    assert args.label.isalnum()
    run(args)

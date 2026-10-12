#!/usr/bin/env python3
"""Verify native table/column history reads before any child Runtime loads."""
import argparse
import os
import resource
from fork_parent_truncate_probe import BootstrapExperiment, namespace_id


def run(binary, selected=None):
    experiment = BootstrapExperiment(binary, 'native_schema_catalog', prototype=6)
    control = experiment.base / 'native-schema-catalog-control'
    os.environ['SEEKDB_NATIVE_SCHEMA_CATALOG_CONTROL'] = str(control)
    try:
        experiment.start()
        cases = [
            ('typed', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "u BIGINT UNSIGNED DEFAULT 18446744073709551615, n INT DEFAULT NULL, "
             "e VARCHAR(100) DEFAULT '', c ENUM('a','b') DEFAULT 'b'", '', (6, 0, 0, 0, 0, 0)),
            ('indexed', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('plain', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('part', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', KEY ix(v), "
             "CONSTRAINT ck CHECK (id >= 0)", ' PARTITION BY HASH(id) PARTITIONS 8', (2, 8, 1, 1, 0, 0)),
            ('part2', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'",
             ' PARTITION BY RANGE(id) SUBPARTITION BY HASH(id) SUBPARTITIONS 2 '
             '(PARTITION p0 VALUES LESS THAN(10), PARTITION p1 VALUES LESS THAN MAXVALUE)',
             (2, 2, 0, 0, 0, 0)),
            ('drop_index', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', KEY ix(v)", '',
             (2, 0, 0, 0, 0, 0)),
            ('fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('self_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', parent_id INT, "
             "KEY parent_idx(parent_id), CONSTRAINT fk FOREIGN KEY(parent_id) REFERENCES t(id)",
             '', (3, 0, 1, 0, 1, 0)),
            ('drop_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 0, 0)),
            ('old_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('history', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('dropped', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', n INT", '', (2, 0, 0, 0, 0, 0)),
            ('generated', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "n INT GENERATED ALWAYS AS (LENGTH(v)) STORED", '', (3, 0, 0, 0, 0, 0)),
            ('lob', "id INT PRIMARY KEY, v VARCHAR(20000) DEFAULT '" + 'x' * 10000 + "'", '',
             (2, 0, 0, 0, 0, 10000)),
        ]
        for label, columns, options, expected in cases:
            if selected and label not in selected:
                continue
            database = f'native_catalog_{label}'
            experiment.sql(f'CREATE DATABASE {database}')
            if label in ('fk', 'drop_fk', 'old_fk'):
                experiment.sql(f'CREATE TABLE {database}.parent(id INT PRIMARY KEY)')
                columns = columns.replace('REFERENCES parent', f'REFERENCES {database}.parent')
            if label == 'self_fk':
                columns = columns.replace('REFERENCES t', f'REFERENCES {database}.t')
            experiment.sql(f'CREATE TABLE {database}.t({columns}){options}')
            table_id = experiment.sql("SELECT t.table_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d "
                                      f"ON t.database_id=d.database_id WHERE d.database_name='{database}' "
                                      "AND t.table_name='t'")[0][0]
            requested_version = 2**63 - 1
            if label == 'history':
                requested_version = experiment.sql(f'SELECT schema_version FROM oceanbase.__all_table '
                                                   f'WHERE table_id={table_id}')[0][0]
                experiment.sql(f'ALTER TABLE {database}.t ADD n INT DEFAULT 17')
            elif label in ('drop_fk', 'old_fk'):
                if label == 'old_fk':
                    requested_version = experiment.sql(f'SELECT schema_version FROM oceanbase.__all_table '
                                                       f'WHERE table_id={table_id}')[0][0]
                experiment.sql(f'ALTER TABLE {database}.t DROP FOREIGN KEY fk')
            elif label == 'drop_index':
                experiment.sql(f'ALTER TABLE {database}.t DROP INDEX ix')
            elif label == 'dropped':
                experiment.sql(f'ALTER TABLE {database}.t DROP COLUMN n')
                stored_columns = experiment.sql(f'SELECT column_name, is_hidden FROM oceanbase.__all_column '
                                                f'WHERE table_id={table_id} ORDER BY column_id')
                assert not any(row[0] == 'n' for row in stored_columns), stored_columns
                expected = (len(stored_columns), *expected[1:])
            name = f'native_catalog_cold_{label}'
            control.write_text(f'{name} {table_id} ' + ' '.join(map(str, expected))
                               + f' {requested_version}\n')
            experiment.sql(f'FORK NAMESPACE {name} FROM ns1')
            control.unlink()
            child_id = namespace_id(experiment, name)
            evidence = [line for log in (experiment.base / 'log').glob('seekdb.log*')
                        for line in log.read_text(errors='replace').splitlines()
                        if f'NATIVE_SCHEMA_CATALOG_FULL namespace={child_id} table={table_id}' in line]
            assert any('ret=0' in line for line in evidence), evidence
            if label == 'indexed':
                trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
                assert f'NATIVE_SCHEMA_CATALOG_INDEXED namespace={child_id} stage=erase_committed ret=0' in trace
            if label == 'typed':
                trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
                assert f'NATIVE_SCHEMA_CATALOG_TYPED namespace={child_id} stage=committed ret=0' in trace
            if label == 'plain':
                borrowed = [line for log in (experiment.base / 'log').glob('seekdb.log*')
                            for line in log.read_text(errors='replace').splitlines()
                            if f'NATIVE_SCHEMA_CATALOG_BORROWED namespace={child_id} ' in line]
                assert any('stage=read_rollback ret=0' in line for line in borrowed), borrowed
            experiment.record('PASS', case=f'native_schema_catalog_{label}', evidence=evidence,
                              inherited_history=True, runtime_slots_loaded=0)
    finally:
        control.unlink(missing_ok=True)
        experiment.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--case', action='append', choices=('typed', 'indexed', 'plain', 'part', 'fk', 'generated', 'lob', 'history', 'dropped', 'part2', 'drop_index', 'self_fk', 'drop_fk', 'old_fk'))
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary, args.case)

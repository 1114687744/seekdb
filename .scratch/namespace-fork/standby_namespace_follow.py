import argparse, contextlib, json, time
from pathlib import Path
import pymysql
from native_schema_catalog_ddl_probe import exercise_constraints, verify_constraints, exercise_foreign_keys, verify_foreign_keys, exercise_partitions, verify_partitions, exercise_views, verify_views, exercise_triggers, verify_triggers, exercise_recovered_triggers, exercise_operations, verify_operations

p=argparse.ArgumentParser(); p.add_argument('--initial-only',action='store_true'); args=p.parse_args()

def connect(port,ns='sys'):
 return pymysql.connect(host='127.0.0.1',port=port,user='root@'+ns,database='oceanbase',autocommit=True,connect_timeout=2,read_timeout=25,write_timeout=25)

def sql(c,text):
 with c.cursor() as q: q.execute(text); return q.fetchall()

def record(case,**facts):
 print(json.dumps(dict(case=case,**facts),ensure_ascii=False),flush=True)

def wait(fn,expected,timeout=40):
 end=time.monotonic()+timeout; last=None
 while time.monotonic()<end:
  try:
   last=fn()
   if last==expected:return last
  except pymysql.MySQLError as e:last=e.args
  time.sleep(.2)
 raise AssertionError((expected,last))

def sync(primary,standby):
 target=int(sql(primary,'select sync_scn from __all_virtual_server_stat')[0][0])
 wait(lambda:int(sql(standby,'select sync_scn from __all_virtual_server_stat')[0][0])>=target,True)

def read_ns(ns,query):
 with connect(42036,ns) as c:return sql(c,query)

with connect(42035) as root,connect(42036) as standby:
 expected=((1,10,16384),)
 query='select id,v,length(note) from replica_ns.t order by id'
 wait(lambda:read_ns('replica_seed',query),expected)
 names=sql(root,'select key_json,value_json from __all_virtual_instance_metadata where collection_id=1')
 nsid=next(int(json.loads(k)['namespace_id']) for k,v in names if json.loads(v)['name']=='replica_seed')
 with connect(42035,'replica_seed') as child:
  logical=int(sql(child,"select tablet_id from __all_table where table_name='t' and database_id=(select database_id from __all_database where database_name='replica_ns')")[0][0])
 physical=(1<<62)|(nsid<<37)|logical
 # The primary now materializes cold namespaces in the background. A copied
 # physical tablet is legitimate; standby reads must not create one absent on
 # the primary.
 assert sql(standby,f'select count(*) from __all_virtual_tablet_info where tablet_id={physical}')[0][0] <= sql(root,f'select count(*) from __all_virtual_tablet_info where tablet_id={physical}')[0][0]
 with connect(42036,'replica_seed') as c:
  try: sql(c,"insert into replica_ns.t values(100,100,'forbidden')")
  except pymysql.MySQLError as e: assert e.args[0]==4688,e.args
  else: raise AssertionError('standby child accepted a write')
 assert sql(standby,f'select count(*) from __all_virtual_tablet_info where tablet_id={physical}')[0][0] <= sql(root,f'select count(*) from __all_virtual_tablet_info where tablet_id={physical}')[0][0]
 record('initial_restore_fork_read_and_write_rejection',namespace_id=nsid,logical_tablet=logical)
 if args.initial_only:raise SystemExit(0)
 sql(root,"insert into replica_ns.t values(2,20,'parent later')")
 sql(root,'FORK NAMESPACE replica_cold FROM ns1')
 sync(root,standby)
 wait(lambda:read_ns('replica_cold',query),((1,10,16384),(2,20,12)))
 record('dynamic_namespace_discovery')
 with connect(42035,'replica_seed') as child:
  sql(child,"insert into replica_ns.t values(3,30,'child value')")
  sync(root,standby)
  wait(lambda:read_ns('replica_seed',query),((1,10,16384),(3,30,11)))
  wait(lambda:sql(standby,f'select count(*) from __all_virtual_tablet_info where tablet_id={physical}'),((1,),))
  record('materialization_transaction_replay_with_lob')
  sql(child,'alter table replica_ns.t add column extra int default 7')
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','select id,extra from replica_ns.t order by id'),((1,7),(3,7)))
  record('child_ddl_refresh')
  sql(child,'CREATE TABLE replica_ns.native_delete(id INT PRIMARY KEY,v INT,body LONGTEXT,'
            'CONSTRAINT delete_ck CHECK(v>=0))')
  sql(child,"INSERT INTO replica_ns.native_delete VALUES(1,7,REPEAT('d',90000))")
  delete_tid=sql(child,"SELECT table_id FROM oceanbase.__all_table WHERE table_name='native_delete'")[0][0]
  deleted_cid=sql(child,f"SELECT column_id FROM oceanbase.__all_column WHERE table_id={delete_tid} AND column_name='v'")[0][0]
  sql(child,'ALTER TABLE replica_ns.native_delete DROP CONSTRAINT delete_ck')
  sql(child,'ALTER TABLE replica_ns.native_delete DROP COLUMN v')
  sql(child,'ALTER TABLE replica_ns.native_delete ADD v INT DEFAULT 23')
  sql(child,'ALTER TABLE replica_ns.native_delete DROP COLUMN body')
  sql(child,'CREATE TABLE replica_ns.native_purge(id INT PRIMARY KEY,a INT,b INT)')
  purge_tid=sql(child,"SELECT table_id FROM oceanbase.__all_table WHERE table_name='native_purge'")[0][0]
  sql(child,'SET SESSION recyclebin=ON')
  sql(child,'DROP TABLE replica_ns.native_purge')
  assert sql(child,f'SELECT COUNT(*) FROM oceanbase.__all_table WHERE table_id={purge_tid}')==((1,),)
  sql(child,'PURGE RECYCLEBIN')
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','SELECT id,v FROM replica_ns.native_delete'),((1,23),))
  wait(lambda:read_ns('replica_seed',f'SELECT is_deleted FROM oceanbase.__all_column_history '
                     f'WHERE table_id={delete_tid} AND column_id={deleted_cid} '
                     'ORDER BY schema_version DESC LIMIT 1'),((1,),))
  wait(lambda:read_ns('replica_seed',f'SELECT COUNT(*) FROM oceanbase.__all_constraint WHERE table_id={delete_tid}'),((0,),))
  wait(lambda:read_ns('replica_seed',f'SELECT COUNT(*) FROM oceanbase.__all_column WHERE table_id={purge_tid}'),((0,),))
  wait(lambda:read_ns('replica_seed',f'SELECT is_deleted FROM oceanbase.__all_table_history '
                     f'WHERE table_id={purge_tid} ORDER BY schema_version DESC LIMIT 1'),((1,),))
  record('native_catalog_delete_replay')
  constraint_evidence=exercise_constraints(lambda statement:sql(child,statement))
  sync(root,standby)
  # Replay SCN can lead this already-loaded Namespace's schema refresh.
  # Wait for the final table shape before checking persistent metadata.
  wait(lambda:read_ns('replica_seed','SELECT id,value FROM native_constraints.state_t'),((1,1),))
  verify_constraints(lambda statement:read_ns('replica_seed',statement),constraint_evidence)
  record('native_catalog_constraint_replay')
  fk_evidence=exercise_foreign_keys(lambda statement:sql(child,statement))
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','SELECT id,a,b FROM native_fk.child ORDER BY id'),((1,11,12),(2,11,12)))
  verify_foreign_keys(lambda statement:read_ns('replica_seed',statement),fk_evidence)
  record('native_catalog_foreign_key_replay')
  partition_evidence=exercise_partitions(lambda statement:sql(child,statement))
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','SELECT * FROM native_part.r ORDER BY id'),((24,3),))
  verify_partitions(lambda statement:read_ns('replica_seed',statement),partition_evidence)
  record('native_catalog_partition_replay')
  view_evidence=exercise_views(lambda statement:sql(child,statement))
  sync(root,standby)
  wait(lambda:read_ns('replica_seed',f'SELECT data_type FROM oceanbase.__all_column WHERE table_id={view_evidence[0]} '
                     "AND column_name='v'"),view_evidence[2])
  wait(lambda:read_ns('replica_seed','SELECT * FROM native_views.v'),((1,7),))
  with connect(42036,'replica_seed') as c:verify_views(lambda statement:sql(c,statement),view_evidence)
  record('native_catalog_view_replay')
  trigger_evidence=exercise_triggers(lambda statement:sql(child,statement))
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','SELECT * FROM native_triggers.renamed ORDER BY id'),((1,4),(2,16),(3,13)))
  with connect(42036,'replica_seed') as c:verify_triggers(lambda statement:sql(c,statement),trigger_evidence)
  record('native_catalog_trigger_replay')
  operation_evidence=exercise_operations(lambda statement:sql(child,statement))
  sync(root,standby)
  wait(lambda:read_ns('replica_seed','SELECT * FROM native_operations.v'),((1,7,19),))
  with connect(42036,'replica_seed') as c:verify_operations(lambda statement:sql(c,statement),operation_evidence)
  record('native_catalog_operation_replay')
  sql(child,'USE replica_ns')
  sql(child,'SET SESSION foreign_key_checks=0')
  sql(child,'CREATE TABLE replica_ns.mock_child(id INT PRIMARY KEY,v INT,KEY ix(v),'
            'CONSTRAINT fk_mock FOREIGN KEY(v) REFERENCES missing_parent(id))')
  sql(child,'SET SESSION foreign_key_checks=1')
  sync(root,standby)
  wait(lambda:read_ns('replica_seed',"SELECT mock_fk_parent_table_name FROM oceanbase.__all_mock_fk_parent_table "
                     "WHERE mock_fk_parent_table_name='missing_parent'"),(('missing_parent',),))
  wait(lambda:read_ns('replica_seed','SELECT COUNT(*) FROM replica_ns.mock_child'),((0,),))
  record('mock_parent_schema_replay')
 sql(root,'truncate table replica_ns.t')
 sql(root,"insert into replica_ns.t values(9,90,'new parent')")
 sql(root,'drop table replica_ns.t')
 sync(root,standby)
 wait(lambda:read_ns('replica_cold',query),((1,10,16384),(2,20,12)))
 wait(lambda:read_ns('replica_seed',query),((1,10,16384),(3,30,11)))
 record('historical_source_after_parent_truncate_and_drop')
 sql(root,'FORK NAMESPACE replica_drop FROM replica_seed')
 sync(root,standby)
 wait(lambda:read_ns('replica_drop',query),((1,10,16384),(3,30,11)))
 sql(root,'DROP NAMESPACE replica_drop')
 sync(root,standby)
 def absent():
  try:
   with connect(42036,'replica_drop'):return False
  except pymysql.MySQLError as e:return e.args[0]==1049
 wait(absent,True);record('replicated_namespace_drop_admission')
 sql(root,'ALTER SYSTEM SWITCHOVER TO STANDBY')
 sql(standby,'ALTER SYSTEM SWITCHOVER TO PRIMARY')
 with connect(42036,'replica_seed') as child:
  verify_constraints(lambda statement:sql(child,statement),constraint_evidence)
  verify_foreign_keys(lambda statement:sql(child,statement),fk_evidence)
  verify_partitions(lambda statement:sql(child,statement),partition_evidence)
  verify_views(lambda statement:sql(child,statement),view_evidence)
  verify_triggers(lambda statement:sql(child,statement),trigger_evidence)
  verify_operations(lambda statement:sql(child,statement),operation_evidence)
  exercise_recovered_triggers(lambda statement:sql(child,statement))
  record('native_catalog_trigger_execution_after_promotion')
  assert sql(child,'SELECT id,v FROM replica_ns.native_delete')==((1,23),)
  assert sql(child,f'SELECT COUNT(*) FROM oceanbase.__all_table WHERE table_id={purge_tid}')==((0,),)
  sql(child,"insert into replica_ns.t values(4,40,'promoted child',8)")
  assert sql(child,'select id,v,extra from replica_ns.t order by id')==((1,10,7),(3,30,7),(4,40,8))
  sql(child,'CREATE TABLE replica_ns.missing_parent(id INT PRIMARY KEY)')
  sql(child,'INSERT INTO replica_ns.missing_parent VALUES(1)')
  sql(child,'INSERT INTO replica_ns.mock_child VALUES(1,1)')
  assert sql(child,'SELECT id,v FROM replica_ns.mock_child')==((1,1),)
  assert sql(child,"SELECT COUNT(*) FROM oceanbase.__all_mock_fk_parent_table "
                   "WHERE mock_fk_parent_table_name='missing_parent'")==((0,),)
  record('mock_parent_resolution_after_promotion')
 sql(standby,'FORK NAMESPACE replica_promoted FROM replica_seed')
 with connect(42036,'replica_cold') as cold:
  sql(cold,"insert into replica_ns.t values(5,50,'promoted cold')")
  assert sql(cold,'select id,v from replica_ns.t order by id')==((1,10),(2,20),(5,50))
 record('promotion_lazy_schema_recovery_and_writes')
 with connect(42036,'replica_promoted') as c:
  assert sql(c,'select id,v from replica_ns.t order by id')==((1,10),(3,30),(4,40))
 record('fork_after_promotion')

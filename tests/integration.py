"""Black-box HTTP tests. The application under test is the compiled C executable."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from urllib.request import Request, urlopen
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]

class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db = Path(self.temp.name)/'test.db'
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0)); self.port=sock.getsockname()[1]
        self.url=f'http://127.0.0.1:{self.port}'
        self.log=open(Path(self.temp.name)/'server.log','wb')
        self.start()
    def start(self):
        self.proc=subprocess.Popen([str(ROOT/'build/makerlab'),'--port',str(self.port),'--db',str(self.db)],cwd=ROOT,stdout=self.log,stderr=self.log)
        for _ in range(150):
            try:
                self.get('/api/health');return
            except OSError:
                if self.proc.poll() is not None: self.fail('C server exited during startup')
                time.sleep(.02)
        self.fail('C server did not start')
    def stop(self):
        self.proc.terminate()
        try:self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait();self.fail('Server shutdown timed out')
    def tearDown(self):
        if self.proc.poll() is None:self.stop()
        self.log.close();self.temp.cleanup()
    def get(self,path='/api/state'):
        with urlopen(self.url+path,timeout=6) as r:return json.load(r)
    def post(self,path,payload=None,headers=None):
        request=Request(self.url+path,data=json.dumps({} if payload is None else payload).encode(),headers=headers or {'Content-Type':'application/json','X-MakerLab':'1'})
        with urlopen(request,timeout=10) as r:return json.load(r)
    def create(self,priority=5,title='Solar rover'):
        return self.post('/api/jobs',{'title':title,'owner':'Team Orbit','priority':priority})['id']
    def action(self,ident,action):return self.post(f'/api/jobs/{ident}/{action}')
    def test_full_three_stage_workflow_and_restart_persistence(self):
        ident=self.create()
        for stage in range(3):
            self.post('/api/dispatch');self.post('/api/dispatch')
            self.assertEqual(self.get()['jobs'][0]['status'],'Ready')
            self.action(ident,'start');self.action(ident,'complete')
            self.assertEqual(self.get()['available'],[1,2,1])
        self.stop();self.start()
        self.assertEqual(self.get()['jobs'][0]['status'],'Completed')
    def test_demo_unsafe_rejection_and_lower_priority_grant(self):
        self.post('/api/demo');self.post('/api/dispatch');state=self.get()
        self.assertEqual(state['jobs'][0]['allocation'],[1,0,1])
        self.assertEqual(state['jobs'][1]['allocation'],[0,0,0])
        self.assertEqual(state['safe_sequence'],[1,2])
        self.assertTrue(any(e['kind']=='Unsafe' for e in state['events']))
    def test_early_start_rejected_and_cancel_releases_resources(self):
        ident=self.create()
        for action in ['start','complete']:
            with self.assertRaises(HTTPError) as error:self.action(ident,action)
            self.assertEqual(error.exception.code,400)
        self.post('/api/dispatch');self.action(ident,'cancel')
        self.assertEqual(self.get()['available'],[1,2,1])
        with self.assertRaises(HTTPError):self.action(ident,'cancel')
    def test_concurrent_dispatch_and_resource_conservation(self):
        for i in range(12):self.create(i%10+1)
        with ThreadPoolExecutor(max_workers=12) as pool:list(pool.map(lambda _:self.post('/api/dispatch'),range(32)))
        state=self.get();self.assertIsNotNone(state['safe_sequence'])
        for r,capacity in enumerate([1,2,1]):
            held=sum(j['allocation'][r] for j in state['jobs'])
            self.assertLessEqual(held,capacity);self.assertEqual(held+state['available'][r],capacity)
    def test_actual_priority_aging_order(self):
        old=self.create(1);new=self.create(9)
        with sqlite3.connect(self.db) as db:db.execute('UPDATE jobs SET queued_at=? WHERE id=?',(time.time()-600,old))
        self.post('/api/dispatch');jobs=self.get()['jobs']
        self.assertEqual(jobs[0]['allocation'],[0,1,0]);self.assertEqual(jobs[1]['allocation'],[0,0,0])
    def test_input_and_json_escaping(self):
        title='Rover "α" <script>alert(1)</script> \\ frame'
        self.create(title=title);self.assertEqual(self.get()['jobs'][0]['title'],title)
        for value in [True,0,11,2.5,'5',None]:
            with self.assertRaises(HTTPError):self.create(value)
        for value in ['', ' '*5, 'x'*81, 'hi\x00there']:
            with self.assertRaises(HTTPError):self.create(title=value)
        with self.assertRaises(HTTPError):self.post('/api/jobs',[])
    def test_capacity_guard_and_demo_preserves_jobs(self):
        for _ in range(100):self.create()
        with self.assertRaises(HTTPError):self.create()
        with self.assertRaises(HTTPError):self.post('/api/demo')
        self.assertEqual(len(self.get()['jobs']),100)
        self.action(1,'cancel');self.create()
    def test_origin_and_custom_header_checks(self):
        for headers in [{'Content-Type':'application/json'},{'Content-Type':'application/json','X-MakerLab':'1','Origin':'https://elsewhere.example'}]:
            with self.assertRaises(HTTPError) as error:self.post('/api/demo',headers=headers)
            self.assertEqual(error.exception.code,403)
    def test_assets_and_unknown_routes(self):
        for path in ['/','/style.css','/app.js']:
            with urlopen(self.url+path) as r:self.assertEqual(r.status,200);self.assertIn("default-src 'self'",r.headers['Content-Security-Policy'])
        with self.assertRaises(HTTPError) as error:self.get('/../../src/server.c')
        self.assertEqual(error.exception.code,404)
    def test_duplicate_length_and_bad_json(self):
        cases=[b'Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}',b'Content-Length: 8\r\n\r\n{"x":  }']
        for tail in cases:
            with socket.create_connection(('127.0.0.1',self.port),timeout=5) as s:
                s.sendall(b'POST /api/jobs HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nX-MakerLab: 1\r\n'+tail)
                self.assertIn(b'400 Bad Request',s.recv(4096))

if __name__=='__main__':unittest.main(verbosity=2)

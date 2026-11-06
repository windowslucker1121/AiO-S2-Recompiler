"""Checks tools/ai against a real window (fake_window.py standing in for the game exe) and the MCP protocol; no game needed.

    tools\\.venv\\Scripts\\python tools\\ai\\tests\\test_gamectl.py -v
    set AI_TEST_FOCUS=1 first to also test send_input(method="send"): it takes the focus for a moment.

Small windows appear near the top-left corner of the screen while it runs; they never take the focus (except with AI_TEST_FOCUS).
A real game running at the same time (another session's oot.exe) is never touched: the stand-in is found by its own command line.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import gamectl  # noqa: E402

PY = sys._base_executable          # the real interpreter (the venv's python.exe is a launcher that starts it as a child)
FAKE = HERE / 'fake_window.py'
GREY, RED, WHITE = (64, 64, 64), (255, 0, 0), (255, 255, 255)
# The game routes of these tests (states.py reads NXRT_AI_STATES_TOML instead of recomp/games/<id>/ai_states.toml).
ROUTES = '''
[defaults]
reach = ["title", "continue"]
ready = { until = "log", pattern = "loaded slot", timeout_s = 10 }
args = ["--pos", "40,40", "--data", "{data}"]
settings = { "diag.watchdog_seconds" = -1 }
env = { NXRT_TEST_FROM_DEFAULTS = "1" }

[route.title]
description = "boot to the title"
steps = [ { wait_for = { until = "log", pattern = "fake: title", timeout_s = 15 } }, { wait_for = { until = "window", timeout_s = 15 } } ]

[route.continue]
description = "load the save"
steps = [ { press_until = { input = ["key Return"], every_s = 1, until = { until = "log", pattern = "loaded slot|new game" }, timeout_s = 10 } } ]

[state.fresh]
description = "no save: a new game"
reach = ["title", { input = ["key Return"] }, { wait_for = { until = "log", pattern = "new game", timeout_s = 10 } }]
settings = { "media.video_max_pictures" = 120 }
'''


class FakeGame(gamectl.Game):
    """The game exe is a .cmd running fake_window.py: game processes this server did not start are the fake's interpreters (by their
    command line; the covering window of test_1 is not a game)."""

    def _is_game(self, image_name, pid):
        if image_name.lower() != Path(PY).name.lower():
            return False
        argv = gamectl.command_line(pid) or []
        return any(Path(a).name == FAKE.name for a in argv) and '--topmost' not in argv


class GameCtl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='gamectl_'))
        exe = cls.tmp / 'oot.cmd'       # stands in for oot.exe; the fake runs as a child of cmd.exe, so the window is found through the process tree
        exe.write_text(f'@set FAKE_EXE_DIR=%~dp0\n@"{PY}" "{FAKE}" %*\n')     # the default run's request folder: the exe's
        # states, bug bundles and the game's routes of this test only (states.py reads these at every call)
        os.environ.update(NXRT_AI_STATES_DIR=str(cls.tmp / 'states'), NXRT_AI_BUGS_DIR=str(cls.tmp / 'bugs'),
                          NXRT_AI_STATES_TOML=str(cls.tmp / 'ai_states.toml'))
        (cls.tmp / 'ai_states.toml').write_text(ROUTES, encoding='utf-8')
        cls.g = FakeGame('oot', exe=exe, run_dir=cls.tmp / 'run')
        cls.g.memory_per_run_gb = 0     # the launch memory check has its own test (test_7); the machine may be busy
        cls.cover = None

    @classmethod
    def tearDownClass(cls):
        cls.g.kill()
        if cls.cover:
            cls.cover.kill()
            cls.cover.wait()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def log_has(self, text, timeout=5, instance=None):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if any(text in ln for ln in self.g.log(tail=1000, instance=instance)['lines']):
                return
            time.sleep(0.1)
        self.fail(f'{text!r} not in the log: {self.g.log(tail=15, instance=instance)["lines"]}')

    def centre(self, instance=None, pid=None):
        img = self.g._capture(self.g._target(instance, pid))[0]
        return img.getpixel((img.width // 2, img.height // 2))

    def test_1_window_capture_input(self):
        g = self.g
        g.launch(args=['--size', '320,240', '--pos', '40,40'], settings={'diag.watchdog_seconds': 0, 'test.enabled': True})
        self.assertTrue(g.wait_for('window', timeout_s=15)['ok'])
        s = g.status()
        self.assertTrue(s['running'])
        self.assertEqual(s['instance'], 'default')
        self.assertEqual(list(s['window']['size']), [320, 240])
        self.assertFalse(s['window']['hung'])
        self.assertIn('free_gb', s['memory'])
        for text in ('env NXRT_DIAG_WATCHDOG_SECONDS=0', 'env NXRT_TEST_ENABLED=1', 'env SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0', "'--size', '320,240'"):
            self.log_has(text)

        # covered by a window of another process (topmost, larger): the capture still shows the game's window, and input goes only to it
        type(self).cover = subprocess.Popen([PY, str(FAKE), '--topmost', '--pos', '20,20', '--size', '400,300'])
        time.sleep(1.0)
        shot = g.screenshot(show=False)
        grey = shot['path']
        self.assertEqual(shot['method'], 'game')                # the game's own frame (request file), like the runtime's
        self.log_has('screenshot request ')
        self.assertEqual(self.centre(), GREY)
        self.assertEqual(g.screenshot(show=False, method='printwindow')['method'], 'printwindow')
        jpeg, info = g.screenshot(crop='0,0,0.5,0.5')
        self.assertEqual(tuple(info['size']), (160, 120))
        self.assertEqual(jpeg[:2], b'\xff\xd8')

        threading.Timer(0.5, g.send_input, [['key R']]).start()
        w = g.wait_for('change', timeout_s=10)
        self.assertTrue(w['ok'], w)
        self.assertEqual(self.centre(), RED)
        red = g.screenshot(show=False)['path']
        self.log_has('keydown vk=0x52 sc=0x13 ext=0')
        self.log_has('keyup vk=0x52')

        g.send_input(['key Up', 'down shift', 'up shift', 'text hi', 'wheel -1', 'rclick 10 20', 'wait 50', 'click 0.5 0.5'])
        for text in ('keydown vk=0x26 sc=0x48 ext=1', 'keydown vk=0x10', 'keyup vk=0x10', 'char h', 'char i', 'wheel -120', 'rdown 10 20', 'ldown 160 120'):
            self.log_has(text)
        self.assertTrue(g.wait_for('log', pattern=r'ldown \d+', timeout_s=5)['ok'])
        self.assertTrue(g.wait_for('visible', timeout_s=5)['ok'])
        time.sleep(0.3)
        self.assertEqual(self.centre(), WHITE)
        with self.assertRaises(ValueError):
            g.send_input(['rel 5 5'])           # raw mouse motion needs method=send

        diff = g.compare(grey, red)
        self.assertFalse(diff['passed'])
        self.assertTrue(Path(diff['diff']).exists())
        white = g.screenshot(show=False)['path']
        self.assertTrue(g.compare(white)['passed'])            # against the window now
        self.assertTrue(g.wait_for('image', ref=white, timeout_s=5)['ok'])

        crash = self.tmp / 'oot_crash.txt'                      # the default run: <exe stem>_crash.txt next to the exe
        crash.write_text('fake crash report\n')
        rep = g.reports()
        self.assertEqual(rep[0]['path'], str(crash))
        self.assertTrue(rep[0]['new'])
        self.assertIn(str(crash), g.status()['reports'])
        crash.unlink()

        k = g.kill()
        self.assertTrue(k['killed'])
        self.assertEqual(k['instances'], ['default'])
        self.assertFalse(g.status()['running'])                 # the covering window is no game
        w = g.wait_for('window', timeout_s=2)
        self.assertFalse(w['ok'])
        self.assertIn('not running', w['why'])

    def test_2_script_run(self):
        g = self.g
        r = g.launch(script=['press A', 'peek 0x7100000000 8 0x10', 'poke 0x7100000000 +8 : 0102', '@pass'], args=['--exit-after', '0.5'],
                     wait=True, timeout_s=30)
        self.assertEqual(r['exit_code'], 0, r)
        self.assertEqual(r['result']['verdict'], 'pass')
        self.assertEqual(r['peeks'][0]['hex'], '0102030405060708')
        self.assertIsNone(r['peeks'][1]['hex'])
        self.assertEqual((g.run_dir / 'peek0.in').read_text().strip(), '0x7100000000 8 0x10')
        script = (g.run_dir / 'script.nxs').read_text().splitlines()
        self.assertEqual(script[0], 'press A')
        self.assertTrue(script[1].startswith('@peek ') and script[2].startswith('@poke ') and script[3] == '@pass')
        self.log_has('env NXRT_TEST_FRAME_SOURCE=tick')
        self.log_has(f'env NXRT_TEST_SCRIPT={g.run_dir / "script.nxs"}')
        self.assertEqual(g.restart()['exit_code'], 0)           # same arguments again
        self.assertTrue(Path(str(g.last.log_path) + '.prev').exists())   # the previous run's log is kept

    def test_3_mcp_protocol(self):
        env = dict(os.environ, NXRT_AI_EXE=str(self.g.exe), NXRT_AI_RUN_DIR=str(self.tmp / 'run_mcp'), NXRT_BOOT_MEMORY_PER_RUN_GB='0')
        p = subprocess.Popen([sys.executable, str(HERE.parent / 'mcp_server.py')], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)

        def call(i, method, params=None):
            p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params or {}}) + '\n')
            p.stdin.flush()
            return json.loads(p.stdout.readline())

        def tool(i, name, **arguments):
            return call(i, 'tools/call', {'name': name, 'arguments': arguments})['result']

        try:
            init = call(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 't', 'version': '0'}})
            self.assertEqual(init['result']['protocolVersion'], '2025-06-18')
            p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
            tools = {t['name']: t for t in call(2, 'tools/list')['result']['tools']}
            self.assertEqual(sorted(tools), sorted(gamectl.TOOLS))
            self.assertEqual(tools['send_input']['inputSchema']['required'], ['steps'])
            self.assertEqual(tools['send_input']['inputSchema']['properties']['steps']['type'], 'array')
            for name in ('status', 'screenshot', 'send_input', 'wait_for', 'log', 'kill', 'reports', 'compare'):     # every tool addresses a run
                self.assertEqual(tools[name]['inputSchema']['properties']['instance']['type'], 'string', name)
                self.assertEqual(tools[name]['inputSchema']['properties']['pid']['type'], 'integer', name)
            self.assertEqual(tools['launch']['inputSchema']['properties']['instance']['type'], 'string')
            self.assertEqual(tools['kill']['inputSchema']['properties']['force']['type'], 'boolean')
            for name in ('capture_bug', 'save_state'):                                       # and the state / bug tools
                self.assertEqual(tools[name]['inputSchema']['properties']['pid']['type'], 'integer', name)
            for name in ('capture_bug', 'save_state', 'load_state', 'bugs'):
                self.assertEqual(tools[name]['inputSchema']['properties']['instance']['type'], 'string', name)
            self.assertEqual(tools['load_state']['inputSchema']['properties']['route']['items'], {})   # route names or step dicts
            st = tool(3, 'status')
            self.assertNotIn('isError', st)
            self.assertEqual(json.loads(st['content'][0]['text'])['game'], 'oot')
            self.assertTrue(call(4, 'tools/call', {'name': 'nope', 'arguments': {}})['result']['isError'])
            self.assertIn('error', call(5, 'no/such/method'))
            self.assertNotIn('isError', tool(6, 'launch', args=['--pos', '40,40'], instance='m1'))
            self.assertIn('"ok": true', tool(7, 'wait_for', until='window', timeout_s=15, instance='m1')['content'][0]['text'])
            shot = tool(8, 'screenshot', instance='m1')['content']
            self.assertEqual((shot[0]['type'], shot[0]['mimeType']), ('image', 'image/jpeg'))
            self.assertIn('"path"', shot[1]['text'])
            self.assertIn('[m1]', json.loads(shot[1]['text'])['title'])
            self.assertTrue(tool(9, 'screenshot', instance='nope')['isError'])      # an instance that is not running is an error
            self.assertIn('"killed"', tool(10, 'kill', instance='m1')['content'][0]['text'])
        finally:
            p.stdin.close()
            p.wait(10)
            p.stdout.close()

    @unittest.skipUnless(os.environ.get('AI_TEST_FOCUS') == '1', 'takes the focus: set AI_TEST_FOCUS=1')
    def test_4_send_input_focus(self):
        g = self.g
        g.launch(args=['--size', '320,240', '--pos', '40,40'])
        self.assertTrue(g.wait_for('window', timeout_s=15)['ok'])
        g.send_input(['key G', 'click 0.25 0.25', 'wheel 1', 'text ok'], method='send')
        for text in ('keydown vk=0x47 sc=0x22 ext=0', 'ldown 80 60', 'wheel 120', 'char o', 'char k'):
            self.log_has(text)
        g.kill()

    def test_5_two_instances(self):
        """Two named runs of this server at once: own folders and arguments; every tool reaches the one it names, by instance or by pid."""
        g = self.g
        a = g.launch(instance='a', args=['--size', '320,240', '--pos', '40,40'])
        b = g.launch(instance='b', args=['--size', '320,240', '--pos', '420,40'])
        try:
            self.assertNotIn('exit_code', a)                     # still running after the start check
            for r, name in ((a, 'a'), (b, 'b')):
                self.assertEqual(Path(r['run_dir']), g.run_dir / name)
                i = r['cmd'].index('--instance')
                self.assertEqual(r['cmd'][i:i + 4], ['--instance', name, '--instance-dir', str(g.run_dir / name)])
                self.assertEqual(Path(r['log']), g.run_dir / name / 'oot.log')
                self.assertTrue(g.wait_for('window', timeout_s=15, instance=name)['ok'])
            s = g.status()
            self.assertEqual(sorted(i['instance'] for i in s['instances'] if i['started_by_this_server']), ['a', 'b'])
            self.assertIn('several', s['note'])
            self.assertNotIn('window', s)                       # no details without a name when two run
            with self.assertRaises(RuntimeError):
                g.screenshot(show=False)                        # which one?
            with self.assertRaises(RuntimeError):
                g.send_input(['key R'])
            self.assertEqual(g.status(instance='b')['window']['title'], 'fake game window [b]')

            threading.Timer(0.5, g.send_input, [['key R']], {'instance': 'a'}).start()     # only a turns red
            self.assertTrue(g.wait_for('change', timeout_s=10, instance='a')['ok'])
            self.assertEqual(self.centre('a'), RED)
            self.assertEqual(self.centre('b'), GREY)
            self.log_has('keydown vk=0x52', instance='a')
            self.assertFalse(any('keydown' in ln for ln in g.log(tail=1000, instance='b')['lines']))
            self.assertTrue(g.wait_for('log', pattern='keydown vk=0x52', timeout_s=5, instance='a')['ok'])
            self.assertFalse(g.wait_for('log', pattern='keydown vk=0x52', timeout_s=1, instance='b')['ok'])

            # by pid: the window's process (the fake under cmd.exe) and the run's root both name b
            shot = g.screenshot(show=False, instance='b')
            self.assertTrue(Path(shot['path']).is_relative_to(g.run_dir / 'b'))
            by_pid = g.screenshot(show=False, pid=shot['pid'])
            self.assertEqual(by_pid['title'], 'fake game window [b]')
            self.assertEqual(g.status(pid=b['pid'])['instance'], 'b')
            g.send_input(['click 0.5 0.5'], pid=shot['pid'])
            self.log_has('ldown 160 120', instance='b')
            with self.assertRaises(RuntimeError):
                g.status(pid=1)                                 # not a game process

            # reports live in each instance's folder
            (g.run_dir / 'b' / 'oot_crash.txt').write_text('fake crash of b\n')
            self.assertEqual(g.reports(instance='a'), 'no crash, freeze or fatal report')
            self.assertEqual(g.reports(instance='b')[0]['path'], str(g.run_dir / 'b' / 'oot_crash.txt'))

            # relaunching a replaces only a; kill by name and by pid
            a2 = g.launch(instance='a', args=['--size', '320,240', '--pos', '40,40'])
            self.assertNotEqual(a2['pid'], a['pid'])
            self.assertTrue(g.wait_for('window', timeout_s=15, instance='a')['ok'])
            self.assertEqual(self.centre('a'), GREY)
            self.assertEqual(g.kill(instance='a')['instance'], 'a')
            self.assertTrue(g.wait_for('exit', timeout_s=5, instance='a')['ok'])
            self.assertEqual([i['instance'] for i in g.status()['instances']], ['b'])
            self.assertEqual(g.status()['instance'], 'b')       # one left: it is the target again
            self.assertTrue(g.kill(pid=shot['pid'])['killed'])
            self.assertFalse(g.status()['running'])
        finally:
            g.kill()

    def test_6_other_sessions_run(self):
        """A run this server did not start (another session's): found with its instance and log from its command line, readable and
        reachable for input, never replaced by launch nor ended by kill without force."""
        g, d = self.g, self.tmp / 'other_x'
        d.mkdir()
        p = subprocess.Popen([PY, str(FAKE), '--instance', 'x', '--instance-dir', str(d), '--log', str(d / 'oot.log'), '--pos', '40,320'])
        try:
            self.assertTrue(g.wait_for('window', timeout_s=15, instance='x')['ok'])
            s = g.status()
            self.assertEqual([(i['instance'], i['pid'], i['started_by_this_server']) for i in s['instances']], [('x', p.pid, False)])
            self.assertEqual(s['instance'], 'x')                # the only game running
            self.assertEqual(g.screenshot(show=False)['title'], 'fake game window [x]')
            self.assertEqual(Path(g.log(instance='x')['path']), d / 'oot.log')
            g.send_input(['key G'], instance='x')
            self.log_has('keydown vk=0x47', instance='x')
            (d / 'oot_crash.txt').write_text('fake crash of x\n')
            self.assertEqual(g.reports(pid=p.pid)[0]['path'], str(d / 'oot_crash.txt'))
            with self.assertRaises(RuntimeError) as cm:
                g.launch(instance='x')                          # its instance name is taken by another's run
            self.assertIn('already running', str(cm.exception))
            self.assertNotIn(p.pid, g.kill()['killed'])         # kill() ends only this server's runs
            with self.assertRaises(RuntimeError) as cm:
                g.kill(instance='x')
            self.assertIn('force', str(cm.exception))
            self.assertIsNone(p.poll())
            self.assertIn(p.pid, g.kill(pid=p.pid, force=True)['killed'])
            self.assertIsNotNone(p.wait(5))
        finally:
            if p.poll() is None:
                p.kill()

    def test_7_start_checks(self):
        """launch refuses an instance it would not have the memory for (the exe's own check, done first), reports a run that the exe
        refused (exit code 7 with its stderr), and rejects names the exe would reject."""
        g = self.g
        g.launch(instance='m1', args=['--pos', '40,40'])
        try:
            self.assertTrue(g.wait_for('window', timeout_s=15, instance='m1')['ok'])
            with self.assertRaises(RuntimeError) as cm:
                g.launch(instance='m2', settings={'boot.memory_per_run_gb': 100000})
            self.assertIn('not enough free memory', str(cm.exception))
            self.assertNotIn('m2', [i['instance'] for i in g.status()['instances']])
            ok = g.launch(instance='m2', settings={'boot.memory_per_run_gb': 0.001}, args=['--pos', '420,40'])
            self.assertNotIn('exit_code', ok)
            r = g.launch(instance='m3', args=['--refuse'])
            self.assertEqual(r['exit_code'], 7)
            self.assertIn('refused to start', r['meaning'])
            self.assertIn('Not enough free memory', r['stderr'])
            self.assertEqual(g.status(instance='m3')['exit_code'], 7)
            for bad in ('two words', '.hidden', 'x' * 41):
                with self.assertRaises(ValueError):
                    g.launch(instance=bad)
            self.assertEqual(gamectl.instance_name('Default'), 'default')
            self.assertEqual(gamectl.instance_name(''), 'default')
            self.assertEqual(gamectl._arg(['a', '--instance', 'q', '--log', 'l'], '--instance'), 'q')
        finally:
            g.kill()

    def test_8_states_and_bugs(self):
        """States and bug bundles per instance: a built-in state, save_state of the run's committed save, load_state of it in a new
        process (the same instance by default), capture_bug from the running game and with none running, bugs, load_state(bug=...)."""
        g = self.g
        try:
            r = g.load_state('fresh', instance='s1', timeout_s=60)
            self.assertTrue(r['ok'], r)
            self.assertEqual((r['instance'], r['save_files']), ('s1', 0))
            save = g.run_dir / 's1' / 'save'
            self.assertEqual(r['save_dir'], str(save))
            for text in (f'env NXRT_FS_SAVE_DIR={save}', 'env NXRT_AI_STATE=fresh', 'env NXRT_MEDIA_VIDEO_MAX_PICTURES=120',
                         'env NXRT_TEST_FROM_DEFAULTS=1', f'env NXRT_BUG_DIR={self.tmp / "bugs"}', "'--data'", "'--instance', 's1'"):
                self.log_has(text, instance='s1')
            self.assertEqual([x['step'] for x in r['steps']], ['title', 'input', 'wait_for'])
            # the game saves; the state holds the committed files only, and the instance
            g.send_input(['key S'], instance='s1')
            self.assertTrue(g.wait_for('log', pattern='fake: saved 1', timeout_s=5, instance='s1')['ok'])
            st = g.save_state('slot1', description='after the first save', instance='s1')
            self.assertEqual((st['save_files'], st['instance']), (2, 's1'), st)
            self.assertEqual(Path(st['from']), save)
            sd = self.tmp / 'states' / 'slot1'
            self.assertTrue((sd / 'save' / 'account' / 'user' / 'slot.sav').exists())
            self.assertFalse(any('.work.' in q.name for q in (sd / 'save' / 'account').iterdir()))
            self.assertTrue((sd / 'shot.png').exists())
            meta = json.loads((sd / 'state.json').read_text())
            self.assertEqual((meta['reach'], meta['instance']), (['title', 'continue'], 's1'))
            self.assertEqual((meta['build']['commit'], meta['build']['dirty']), ('fake0123', True))
            self.assertEqual(meta['settings'].get('media.video_max_pictures'), 120)
            self.assertNotIn('fs.save_dir', meta['settings'])
            with self.assertRaises(RuntimeError):
                g.save_state('slot1', instance='s1')                # exists: overwrite=true replaces it
            lst = {x['name']: x for x in g.states()['states']}
            self.assertEqual(lst['fresh']['kind'], 'built-in')
            self.assertEqual((lst['slot1']['kind'], lst['slot1']['instance'], lst['slot1']['save_files']), ('saved', 's1', 2))
            self.assertTrue(lst['slot1']['same_exe'])
            # back to the saved moment: a new process of the same instance gets the state's save and finds the slot
            first = g.runs['s1'].proc.pid
            r = g.load_state('slot1', timeout_s=60)
            self.assertTrue(r['ok'], r)
            self.assertEqual(r['instance'], 's1')
            self.assertNotEqual(r['pid'], first)
            self.assertEqual(r['steps'][-1]['step'], 'ready')
            self.log_has('fake: loaded slot saved 1', instance='s1')
            # a bug bundle from the running game (request file in the instance folder), with what this side adds
            b = g.capture_bug(note='the door does not open', name='door', instance='s1')
            self.assertEqual((b['by'], b['instance']), ('game', 's1'), b)
            self.assertIn('-s1-door', b['id'])
            for f in ('note.txt', 'bug.json', 'env.txt', 'save', 'window.png', 'mcp.json', 'stdout.txt'):
                self.assertIn(f, b['files'])
            self.assertFalse(list((g.run_dir / 's1').glob('capture_bug.*')), 'request files left behind')
            self.log_has('bug: bundle complete', instance='s1')
            lst = g.bugs(instance='s1')['bugs']
            self.assertEqual(lst[0]['id'], b['id'])
            self.assertEqual((lst[0]['note'], lst[0]['state'], lst[0]['instance'], lst[0]['save_files']),
                             ('the door does not open', 'slot1', 's1', 2))
            self.assertEqual(g.bugs(instance='nope')['bugs'], [])
            jpg, info = g.bugs(id='door')
            self.assertEqual(jpg[:2], b'\xff\xd8')
            self.assertEqual((info['note'], info['mcp']['state'], info['mcp']['instance']), ('the door does not open', 'slot1', 's1'))
            # the run not running: the bundle comes from its last files
            g.kill(instance='s1')
            time.sleep(1.1)                                         # another second: its own folder name
            b2 = g.capture_bug(note='after the exit', instance='s1')
            self.assertEqual((b2['by'], b2['instance']), ('this server', 's1'))
            self.assertIn('not running', b2['why'])
            for f in ('note.txt', 'bug.json', 'log.txt', 'save', 'env.txt', 'mcp.json'):
                self.assertIn(f, b2['files'])
            self.assertIn('fake: loaded slot', (Path(b2['bundle']) / 'log.txt').read_text())
            self.assertIn('NXRT_AI_STATE=slot1', (Path(b2['bundle']) / 'env.txt').read_text())
            self.assertEqual(g.bugs(id='latest', show=False)['id'], b2['id'])
            # from a bundle: its save data, its env, its instance, the reach of the state its run came from
            r = g.load_state(bug='door', timeout_s=60)
            self.assertTrue(r['ok'], r)
            self.assertEqual(r['instance'], 's1')
            self.log_has('env NXRT_AI_STATE=bug:' + b['id'], instance='s1')
            self.log_has('fake: loaded slot saved 1', instance='s1')
            # the same bundle in another instance, beside it: its own save folder
            r = g.load_state(bug='door', instance='s2', timeout_s=60)
            self.assertTrue(r['ok'], r)
            self.assertEqual((r['instance'], r['save_dir']), ('s2', str(g.run_dir / 's2' / 'save')))
            self.log_has('fake: loaded slot saved 1', instance='s2')
            self.assertEqual(sorted(i['instance'] for i in g.status()['instances']), ['s1', 's2'])
            st = g.save_state('from_bug', from_bug='door')
            self.assertEqual((st['save_files'], st['instance']), (2, 's1'))
            self.assertEqual(json.loads((self.tmp / 'states' / 'from_bug' / 'state.json').read_text())['from_bug'], 'door')
            g.kill()
            # the default run: its save folder is the run dir's
            r = g.load_state('fresh', timeout_s=60)
            self.assertTrue(r['ok'], r)
            self.assertEqual((r['instance'], r['save_dir']), ('default', str(g.run_dir / 'save')))
            with self.assertRaises(ValueError):
                g.load_state('no_such_state', reach=False)
        finally:
            g.kill()

    def test_9_game_routes_parse(self):
        """The checked-in routes of every game: valid TOML, every state's and route's steps name known routes and step kinds."""
        import states
        for toml in (HERE.parents[2] / 'recomp' / 'games').glob('*/ai_states.toml'):
            os.environ['NXRT_AI_STATES_TOML'] = str(toml)
            try:
                ai = gamectl.Game(toml.parent.name, exe=self.g.exe, run_dir=self.tmp / 'run_routes')._game_ai()
            finally:
                os.environ['NXRT_AI_STATES_TOML'] = str(self.tmp / 'ai_states.toml')
            routes = ai.get('route') or {}

            def check(steps, where):
                for st in steps:
                    if isinstance(st, str):
                        self.assertIn(st, routes, f'{toml}: {where} names route {st!r}')
                        continue
                    kinds = [k for k in states.STEP_KINDS if k in st]
                    self.assertEqual(len(kinds), 1, f'{toml}: {where}: step {st}')
                    if kinds[0] == 'route':
                        self.assertIn(st['route'], routes, f'{toml}: {where}')
            check((ai.get('defaults') or {}).get('reach', []), 'defaults')
            for n, r in routes.items():
                check(r.get('steps', []), f'route {n}')
            for n, st in (ai.get('state') or {}).items():
                check(st.get('reach', []), f'state {n}')


if __name__ == '__main__':
    unittest.main()

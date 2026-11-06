"""Checks tools/ai against a real window (fake_window.py standing in for the game exe) and the MCP protocol; no game needed.

    tools\\.venv\\Scripts\\python tools\\ai\\tests\\test_gamectl.py -v
    set AI_TEST_FOCUS=1 first to also test send_input(method="send"): it takes the focus for a moment.

Small windows appear near the top-left corner of the screen while it runs; they never take the focus (except with AI_TEST_FOCUS).
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


class GameCtl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='gamectl_'))
        exe = cls.tmp / 'oot.cmd'       # stands in for oot.exe; the fake runs as a child of cmd.exe, so the window is found through the process tree
        exe.write_text(f'@set FAKE_EXE_DIR=%~dp0\n@"{PY}" "{FAKE}" %*\n')
        # states, bug bundles and the game's routes of this test only (states.py reads these at every call)
        os.environ.update(NXRT_AI_STATES_DIR=str(cls.tmp / 'states'), NXRT_AI_BUGS_DIR=str(cls.tmp / 'bugs'),
                          NXRT_AI_STATES_TOML=str(cls.tmp / 'ai_states.toml'))
        (cls.tmp / 'ai_states.toml').write_text(ROUTES, encoding='utf-8')
        cls.g = gamectl.Game('oot', exe=exe, run_dir=cls.tmp / 'run')
        cls.cover = None

    @classmethod
    def tearDownClass(cls):
        cls.g.kill()
        if cls.cover:
            cls.cover.kill()
            cls.cover.wait()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def log_has(self, text, timeout=5):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if any(text in ln for ln in self.g.log(tail=1000)['lines']):
                return
            time.sleep(0.1)
        self.fail(f'{text!r} not in the log: {self.g.log(tail=15)["lines"]}')

    def centre(self):
        img = self.g._capture()[0]
        return img.getpixel((img.width // 2, img.height // 2))

    def test_1_window_capture_input(self):
        g = self.g
        g.launch(args=['--size', '320,240', '--pos', '40,40'], settings={'diag.watchdog_seconds': 0, 'test.enabled': True})
        self.assertTrue(g.wait_for('window', timeout_s=15)['ok'])
        s = g.status()
        self.assertTrue(s['running'])
        self.assertEqual(list(s['window']['size']), [320, 240])
        self.assertFalse(s['window']['hung'])
        for text in ('env NXRT_DIAG_WATCHDOG_SECONDS=0', 'env NXRT_TEST_ENABLED=1', 'env SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0', "'--size', '320,240'"):
            self.log_has(text)

        # covered by a window of another process (topmost, larger): the capture still shows the game's window, and input goes only to it
        type(self).cover = subprocess.Popen([PY, str(FAKE), '--topmost', '--pos', '20,20', '--size', '400,300'])
        time.sleep(1.0)
        grey = g.screenshot(show=False)['path']
        self.assertEqual(self.centre(), GREY)
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

        crash = self.tmp / 'oot_crash.txt'                      # <exe stem>_crash.txt next to the exe
        crash.write_text('fake crash report\n')
        rep = g.reports()
        self.assertEqual(rep[0]['path'], str(crash))
        self.assertTrue(rep[0]['new'])
        self.assertIn(str(crash), g.status()['reports'])
        crash.unlink()

        k = g.kill()
        self.assertTrue(k['killed'])
        self.assertFalse(g.status()['running'])
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
        self.assertTrue(Path(str(g.log_path) + '.prev').exists())   # the previous run's log is kept

    def test_3_mcp_protocol(self):
        env = dict(os.environ, NXRT_AI_EXE=str(self.g.exe), NXRT_AI_RUN_DIR=str(self.tmp / 'run_mcp'))
        p = subprocess.Popen([sys.executable, str(HERE.parent / 'mcp_server.py')], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)

        def call(i, method, params=None):
            p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params or {}}) + '\n')
            p.stdin.flush()
            return json.loads(p.stdout.readline())

        try:
            init = call(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 't', 'version': '0'}})
            self.assertEqual(init['result']['protocolVersion'], '2025-06-18')
            p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
            tools = {t['name']: t for t in call(2, 'tools/list')['result']['tools']}
            self.assertEqual(sorted(tools), sorted(gamectl.TOOLS))
            self.assertEqual(tools['send_input']['inputSchema']['required'], ['steps'])
            self.assertEqual(tools['send_input']['inputSchema']['properties']['steps']['type'], 'array')
            st = call(3, 'tools/call', {'name': 'status', 'arguments': {}})['result']
            self.assertNotIn('isError', st)
            self.assertEqual(json.loads(st['content'][0]['text'])['game'], 'oot')
            self.assertTrue(call(4, 'tools/call', {'name': 'nope', 'arguments': {}})['result']['isError'])
            self.assertIn('error', call(5, 'no/such/method'))
            self.assertNotIn('isError', call(6, 'tools/call', {'name': 'launch', 'arguments': {'args': ['--pos', '40,40']}})['result'])
            self.assertIn('"ok": true', call(7, 'tools/call', {'name': 'wait_for', 'arguments': {'until': 'window', 'timeout_s': 15}})['result']['content'][0]['text'])
            shot = call(8, 'tools/call', {'name': 'screenshot', 'arguments': {}})['result']['content']
            self.assertEqual((shot[0]['type'], shot[0]['mimeType']), ('image', 'image/jpeg'))
            self.assertIn('"path"', shot[1]['text'])
            self.assertIn('"killed"', call(9, 'tools/call', {'name': 'kill', 'arguments': {}})['result']['content'][0]['text'])
        finally:
            p.stdin.close()
            p.wait(10)
            p.stdout.close()

    def test_5_states_and_bugs(self):
        g = self.g
        # a built-in state: an empty save folder of this server's own, the game's defaults and the state's settings, the reach steps
        r = g.load_state('fresh', timeout_s=60)
        self.assertTrue(r['ok'], r)
        self.assertEqual(r['save_files'], 0)
        save = g.run_dir / 'save'
        self.assertEqual(r['save_dir'], str(save))
        for text in (f'env NXRT_FS_SAVE_DIR={save}', 'env NXRT_AI_STATE=fresh', 'env NXRT_MEDIA_VIDEO_MAX_PICTURES=120',
                     'env NXRT_TEST_FROM_DEFAULTS=1', f'env NXRT_BUG_DIR={self.tmp / "bugs"}', "'--data'"):
            self.log_has(text)
        self.assertEqual([s['step'] for s in r['steps']], ['title', 'input', 'wait_for'])
        # the game saves; the state holds the committed files only
        g.send_input(['key S'])
        self.assertTrue(g.wait_for('log', pattern='fake: saved 1', timeout_s=5)['ok'])
        st = g.save_state('slot1', description='after the first save')
        self.assertEqual(st['save_files'], 2, st)
        self.assertEqual(Path(st['from']), save)
        sd = self.tmp / 'states' / 'slot1'
        self.assertTrue((sd / 'save' / 'account' / 'user' / 'slot.sav').exists())
        self.assertFalse(any('.work.' in p.name for p in (sd / 'save' / 'account').iterdir()))
        self.assertTrue((sd / 'shot.png').exists())
        meta = json.loads((sd / 'state.json').read_text())
        self.assertEqual(meta['reach'], ['title', 'continue'])
        self.assertEqual(meta['build']['commit'], 'fake0123')
        self.assertTrue(meta['build']['dirty'])
        self.assertEqual(meta['settings'].get('media.video_max_pictures'), 120)
        self.assertNotIn('fs.save_dir', meta['settings'])
        with self.assertRaises(RuntimeError):
            g.save_state('slot1')                                   # exists: overwrite=true replaces it
        lst = {s['name']: s for s in g.states()['states']}
        self.assertEqual(lst['fresh']['kind'], 'built-in')
        self.assertEqual(lst['slot1']['kind'], 'saved')
        self.assertTrue(lst['slot1']['same_exe'])
        self.assertEqual(lst['slot1']['save_files'], 2)
        # back to the saved moment: a new process gets the state's save and finds the slot
        r = g.load_state('slot1', timeout_s=60)
        self.assertTrue(r['ok'], r)
        self.assertEqual(r['steps'][-1]['step'], 'ready')
        self.log_has('fake: loaded slot saved 1')
        # a bug bundle from the running game (request file), with what this side adds
        b = g.capture_bug(note='the door does not open', name='door')
        self.assertEqual(b['by'], 'game', b)
        for f in ('note.txt', 'bug.json', 'env.txt', 'save', 'window.png', 'mcp.json', 'stdout.txt'):
            self.assertIn(f, b['files'])
        self.assertFalse(list(self.tmp.glob('capture_bug.*')), 'request files left behind')
        self.log_has('bug: bundle complete')
        lst = g.bugs()['bugs']
        self.assertEqual(lst[0]['id'], b['id'])
        self.assertEqual((lst[0]['note'], lst[0]['state'], lst[0]['save_files']), ('the door does not open', 'slot1', 2))
        jpg, info = g.bugs(id='door')
        self.assertEqual(jpg[:2], b'\xff\xd8')
        self.assertEqual(info['note'], 'the door does not open')
        self.assertEqual(info['mcp']['state'], 'slot1')
        # no game running: the bundle comes from the last run's files
        g.kill()
        time.sleep(1.1)                                             # another second: its own folder name
        b2 = g.capture_bug(note='after the exit')
        self.assertEqual(b2['by'], 'this server')
        self.assertIn('not running', b2['why'])
        for f in ('note.txt', 'bug.json', 'log.txt', 'save', 'env.txt', 'mcp.json'):
            self.assertIn(f, b2['files'])
        self.assertIn('fake: loaded slot', (Path(b2['bundle']) / 'log.txt').read_text())
        self.assertIn('NXRT_AI_STATE=slot1', (Path(b2['bundle']) / 'env.txt').read_text())
        self.assertEqual(g.bugs(id='latest', show=False)['id'], b2['id'])
        # from a bundle: its save data, its env, the reach of the state its run came from
        r = g.load_state(bug='door', timeout_s=60)
        self.assertTrue(r['ok'], r)
        self.log_has('env NXRT_AI_STATE=bug:' + b['id'])
        self.log_has('fake: loaded slot saved 1')
        st = g.save_state('from_bug', from_bug='door')
        self.assertEqual(st['save_files'], 2)
        self.assertEqual(json.loads((self.tmp / 'states' / 'from_bug' / 'state.json').read_text())['from_bug'], 'door')
        with self.assertRaises(ValueError):
            g.load_state('no_such_state', reach=False)
        g.kill()

    def test_6_game_routes_parse(self):
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
            for n, s in (ai.get('state') or {}).items():
                check(s.get('reach', []), f'state {n}')

    @unittest.skipUnless(os.environ.get('AI_TEST_FOCUS') == '1', 'takes the focus: set AI_TEST_FOCUS=1')
    def test_4_send_input_focus(self):
        g = self.g
        g.launch(args=['--size', '320,240', '--pos', '40,40'])
        self.assertTrue(g.wait_for('window', timeout_s=15)['ok'])
        g.send_input(['key G', 'click 0.25 0.25', 'wheel 1', 'text ok'], method='send')
        for text in ('keydown vk=0x47 sc=0x22 ext=0', 'ldown 80 60', 'wheel 120', 'char o', 'char k'):
            self.log_has(text)
        g.kill()


if __name__ == '__main__':
    unittest.main()

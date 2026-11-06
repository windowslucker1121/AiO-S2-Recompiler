"""MCP server (stdio, JSON-RPC 2.0 lines) exposing gamectl.Game's tools; with arguments, the same tools as a CLI.

  python tools/ai/mcp_server.py                          MCP server (registered in the repo's .mcp.json as "game")
  python tools/ai/mcp_server.py help                     list the tools
  python tools/ai/mcp_server.py <tool> [key=value ...]   run one tool; values are JSON when they parse (steps='["key a"]'), else text

Only what Claude Code uses is implemented: initialize, ping, tools/list, tools/call (a tool error comes back as isError).
The CLI is a new process per call: it finds the game by the exe's file name, not by the process a previous call started.
"""
import base64
import inspect
import json
import sys
import typing
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gamectl  # noqa: E402

JSON_TYPES = {str: 'string', int: 'integer', float: 'number', bool: 'boolean', list: 'array', dict: 'object'}


def tool_schema(name):
    fn = getattr(gamectl.Game, name)
    props, required = {}, []
    for p in list(inspect.signature(fn).parameters.values())[1:]:
        t = typing.get_origin(p.annotation) or p.annotation
        props[p.name] = {'type': JSON_TYPES[t]} | ({'items': {} if p.name in gamectl.ANY_ITEMS else {'type': 'string'}} if t is list else {})
        if p.default is inspect.Parameter.empty:
            required.append(p.name)
        elif p.default is not None:
            props[p.name]['default'] = p.default
    return {'name': name, 'description': inspect.getdoc(fn), 'inputSchema': {'type': 'object', 'properties': props, 'required': required}}


def content(result):
    """A tool's return value as MCP content: a tuple is several items, bytes a JPEG picture, anything else JSON text."""
    items = result if isinstance(result, tuple) else (result,)
    return [{'type': 'image', 'data': base64.b64encode(it).decode(), 'mimeType': 'image/jpeg'} if isinstance(it, bytes)
            else {'type': 'text', 'text': it if isinstance(it, str) else json.dumps(it, indent=1, default=str)} for it in items]


def serve():
    out, game = sys.stdout.buffer, None
    sys.stdout = sys.stderr                                     # a stray print must not corrupt the protocol stream
    for line in sys.stdin.buffer:
        if not line.strip():
            continue
        msg = json.loads(line)
        if 'id' not in msg or 'method' not in msg:              # notifications and replies need no answer
            continue
        method, params = msg['method'], msg.get('params') or {}
        reply = {'jsonrpc': '2.0', 'id': msg['id']}
        if method == 'initialize':
            reply['result'] = {'protocolVersion': params.get('protocolVersion', '2025-06-18'), 'capabilities': {'tools': {}},
                               'serverInfo': {'name': 'game', 'version': '1'},
                               'instructions': 'Drives the game exe of this repo (game id from NXRT_AI_GAME, default oot). Start with status. '
                                               'The exe has no live control channel: the runtime test tooling (pad scripts, peek/poke, probes) '
                                               'runs through launch(script=...); live input is send_input to the game window. '
                                               'states / load_state start the game at a saved moment (save data + a scripted way back, not a '
                                               'process snapshot); capture_bug / bugs are bundles of a moment (the player presses F9 in game).'}
        elif method == 'ping':
            reply['result'] = {}
        elif method == 'tools/list':
            reply['result'] = {'tools': [tool_schema(n) for n in gamectl.TOOLS]}
        elif method == 'tools/call':
            try:
                if params.get('name') not in gamectl.TOOLS:
                    raise ValueError(f'unknown tool {params.get("name")!r}')
                game = game or gamectl.Game()
                reply['result'] = {'content': content(getattr(game, params['name'])(**(params.get('arguments') or {})))}
            except Exception as e:                              # noqa: BLE001  (any failure is the tool's answer)
                reply['result'] = {'content': [{'type': 'text', 'text': f'{type(e).__name__}: {e}'}], 'isError': True}
        else:
            reply['error'] = {'code': -32601, 'message': f'method not found: {method}'}
        out.write(json.dumps(reply).encode() + b'\n')
        out.flush()


def cli(argv):
    if argv[0] not in gamectl.TOOLS:
        for n in gamectl.TOOLS:
            print(f'{n:12} {inspect.getdoc(getattr(gamectl.Game, n)).splitlines()[0]}')
        return 0 if argv[0] == 'help' else 2
    kwargs = {}
    for a in argv[1:]:
        k, _, v = a.partition('=')
        try:
            kwargs[k] = json.loads(v)
        except ValueError:
            kwargs[k] = v
    for it in content(getattr(gamectl.Game(), argv[0])(**kwargs)):
        print(it.get('text', '(picture: see "path")'))
    return 0


if __name__ == '__main__':
    sys.exit(cli(sys.argv[1:]) if sys.argv[1:] else serve())

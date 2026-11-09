"""`bl .+8; .word d` (read a PC-relative word through x30) lifts as a tail call, not a call that comes back into the data word.
Run: python recomp/nxrecomp/lift/test_bl_data_word.py"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from nxrecomp.lift.a64 import Fn  # noqa: E402

BASE = 0x7700000000


class Env:
    def __init__(self, words):
        self.words = words

    def word(self, addr):
        return self.words.get(addr)

    def call(self, target):
        return 'f_%x(c);' % target


def lift_bl(data_word):
    pc = BASE + 0x14b4
    fn = Fn(BASE + 0x14b0, BASE + 0x14bc, Env({pc: 0x94000002, pc + 4: data_word}))
    return fn.lift(0x94000002, pc)


if __name__ == '__main__':
    # the rtld word: an offset, not an instruction -> tail call
    assert lift_bl(0x00001b48) == 'c->x[30] = 0x77000014b8ULL; { f_77000014bc(c); return; }', lift_bl(0x00001b48)
    # a real instruction after the bl (mov x0, x30): an ordinary call that comes back
    assert lift_bl(0xaa1e03e0) == 'c->x[30] = 0x77000014b8ULL; f_77000014bc(c);'
    # the SDK's deliberate UDF #0xdefe and udf #0 stay traps after an ordinary call
    assert lift_bl(0xe7ffdefe).endswith('f_77000014bc(c);')
    assert lift_bl(0).endswith('f_77000014bc(c);')
    # the boot check reports a call returning into data, but not the idiom the lifter handles
    from nxrecomp.bootcheck import calls_into_data

    class Img:
        base, text = BASE, (0, 16)

        def __init__(self, ws):
            self.ws = ws

        def words(self):
            return self.ws
    assert calls_into_data(Img((0x94000002, 0x00001b48, 0xd65f03c0))) == []          # bl .+8; .word
    assert calls_into_data(Img((0x94000010, 0x00001b48, 0xd65f03c0))) == [BASE]      # bl elsewhere; .word
    assert calls_into_data(Img((0x94000010, 0xe7ffdefe))) == []                      # noreturn call; UDF #0xdefe
    print('ok')

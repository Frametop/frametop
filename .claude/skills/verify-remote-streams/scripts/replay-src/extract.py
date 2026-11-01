"""Copy ft-stream.cpp's struct HostInput into a harness include, with the relay's and the pointer
helper's socket names swapped for this run's own (so nothing reaches the live ones)."""
import sys
src, dst, tag = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
start = text.index('struct HostInput {')
end = text.index('\n};\n', start) + 4
body = text[start:end]
old = '*kRelay = FT_SOCK_RELAY, *kHelper = FT_SOCK_POINTER_HELPER'
assert body.count(old) == 1, 'socket names moved'
body = body.replace(old, f'*kRelay = "fttest_relay_{tag}", *kHelper = "fttest_helper_{tag}"')
open(dst, 'w').write(body)

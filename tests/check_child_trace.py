"""Audit plain host strace: no post-fork allocator/open/directory syscalls.
Fork's libc bookkeeping (set_robust_list) is allowed before module child code.
Does not claim syscall tracing proves absence of userspace libc locks.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
count = 0
for path in sorted(root.glob('child.*')):
    text = path.read_text()
    lines = text.splitlines()
    target = next((i for i, line in enumerate(lines)
                   if re.match(r'execve\("/proc/self/fd/\d+"', line)), None)
    if target is None:
        continue
    # Child trace files start at fork return, unlike owner threads that have
    # open/proc enumeration and the clone syscall in their own trace file.
    pre = lines[:target]
    calls = [re.match(r'(\w+)\(', s).group(1) for s in pre if re.match(r'\w+\(', s)]
    allowed = {'set_robust_list', 'rt_sigprocmask', 'dup2', 'close'}
    assert set(calls) <= allowed, (path, calls)
    assert calls.count('dup2') == 3, (path, calls)
    fd = int(re.search(r'/proc/self/fd/(\d+)', lines[target]).group(1))
    assert fd >= 3, (path, fd)
    assert not any(re.match(rf'close\({fd}\)', s) for s in pre), (path, 'closed exec fd')
    assert any('rt_sigprocmask(SIG_SETMASK, []' in s or
               'rt_sigprocmask(SIG_SETMASK, [USR1]' in s for s in pre), path
    if '= -1 ' in lines[target]:
        after = lines[target + 1:]
        assert any('exit_group(127)' in s for s in after), path
        assert not any(s.startswith('execve(') for s in after), (path, 'exec fallback')
    count += 1
assert count >= 80, count
print(f'PASS: {count} actual helper exec attempts; child syscalls allowlisted, '
      'FD >=3 kept open, signal mask explicit, exec failures have no fallback.')

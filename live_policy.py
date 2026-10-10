"""Application-tree selection. Explicit exclusions override every allow source."""
import re


def compile_rules(include, exclude):
    return (re.compile(include, re.I) if include else None,
            re.compile(exclude, re.I) if exclude else None)


def decide(name, overrides, rules):
    name = name.lower()
    inc, exc = rules
    if name == 'teamspeak.exe':
        return False, 'TeamSpeak исключён'
    if exc and exc.search(name):
        return False, 'exclude'
    if name in overrides:
        return bool(overrides[name]), 'вручную'
    return bool(inc and inc.search(name)), 'include' if inc and inc.search(name) else 'не выбран'


def resolve(processes, overrides, rules, own_pids=()):
    by_pid = {p['pid']: p for p in processes}
    own_pids = set(own_pids)
    def ancestors(pid):
        seen = {pid}
        while pid in by_pid:
            pid = by_pid[pid].get('ppid', 0)
            if not pid or pid in seen:
                break
            seen.add(pid)
            yield pid
    chosen = {p['pid'] for p in processes if p['pid'] not in own_pids and decide(p['name'], overrides, rules)[0]}
    roots = sorted(pid for pid in chosen if not any(a in chosen for a in ancestors(pid)))
    accepted, blocked = [], {}
    for root in roots:
        conflicts = []
        for p in processes:
            if root not in ancestors(p['pid']):
                continue
            name = p['name'].lower()
            explicit_deny = name == 'teamspeak.exe' or p['pid'] in own_pids or overrides.get(name) is False or (rules[1] and rules[1].search(name))
            if explicit_deny:
                conflicts.append(name)
        if conflicts:
            blocked[by_pid[root]['name'].lower()] = sorted(set(conflicts))
        else:
            accepted.append(root)
    if len(accepted) > 64:
        return [], {'*': ['Выбрано больше 64 деревьев процессов']}
    return accepted, blocked

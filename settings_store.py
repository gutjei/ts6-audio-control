"""Persistent application settings keyed by normalized executable name, never PID."""
import json
import math
from live_policy import compile_rules


def normalize(settings):
    if not isinstance(settings, dict):
        raise ValueError('Настройки должны быть объектом JSON')
    include, exclude = settings['include'], settings['exclude']
    if not isinstance(include, str) or not isinstance(exclude, str):
        raise ValueError('Regexp должны быть строками')
    compile_rules(include, exclude)
    overrides, volumes = settings.get('overrides', {}), settings.get('volumes', {})
    for mapping in (overrides, volumes):
        if not isinstance(mapping, dict) or any(not isinstance(k, str) or not k or '/' in k or '\\' in k for k in mapping):
            raise ValueError('Настройки приложений должны использовать имена exe')
    if any(type(v) is not bool for v in overrides.values()):
        raise ValueError('Ручной выбор должен быть true/false')
    if type(settings.get('minimize_to_tray', False)) is not bool:
        raise ValueError('Сворачивание в трей должно быть true/false')
    if type(settings.get('auto_connect', False)) is not bool:
        raise ValueError('Автоподключение должно быть true/false')
    if any(type(v) not in (int, float) or not math.isfinite(v) or not 0 <= v <= 200 for v in volumes.values()):
        raise ValueError('Громкость должна быть числом от 0 до 200 процентов')
    return {**settings, 'include': include, 'exclude': exclude,
            'overrides': {k.lower(): v for k, v in overrides.items()},
            'volumes': {k.lower(): float(v) for k, v in volumes.items()}}


def load(path):
    return normalize(json.loads(path.read_text(encoding='utf-8')))


def save(path, settings):
    clean = normalize(settings)
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(clean, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')
    temp.replace(path)
    return clean


def gains_for(processes, pids, settings):
    names = {p['pid']: p['name'].lower() for p in processes}
    return [settings['volumes'].get(names[pid], 100) / 100 for pid in pids]

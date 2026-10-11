"""Version profiles for recovering the format of an existing TeamSpeak capture."""
import hashlib
from pathlib import Path

SUPPORTED_SHA256 = 'a302bcc4c6ed341ecb922d0e3c29201f5bb385705edbd6f6d6e227eb7b505198'


def profile_for(executable):
    if not executable:
        return None
    module = Path(executable).parent / 'TeamSpeak.dll'
    try:
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
    except OSError:
        return None
    if digest != SUPPORTED_SHA256:
        return None
    return {'path': str(module), 'entry': 0x1aee9e0, 'format': 0xc8, 'capture': 0xe0,
            'callsite': 0x1aeeaba,
            'signature': '8bf885c00f88810000000fb786ca000000'}

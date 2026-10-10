"""Dedicated process with no render streams or children, used as a silent clock."""
import os
from pathlib import Path
import sys
import threading


def main(args=()):
    if len(args) == 2 and args[0] == '--ready-file':
        ready = Path(args[1])
        temporary = ready.with_suffix('.tmp')
        temporary.write_text(str(os.getpid()), encoding='ascii')
        temporary.replace(ready)
    threading.Event().wait()


if __name__ == '__main__':
    main(sys.argv[1:])

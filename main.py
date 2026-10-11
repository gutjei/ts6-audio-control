"""Executable entry point; diagnostic mode does not open the control panel."""
import sys


def main():
    if sys.argv[1:2] == ['--diagnose']:
        if len(sys.argv) != 3:
            return 2
        from diagnostics import run
        return run(sys.argv[2])
    from audio_router_gui import main as gui_main
    return gui_main()


if __name__ == '__main__':
    raise SystemExit(main())

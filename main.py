"""Executable entry point. Handle the silent worker before importing the GUI."""
import sys


def main():
    if sys.argv[1:2] == ['--silent-clock']:
        from silent_clock import main as clock_main
        clock_main(sys.argv[2:])
        return 0
    if sys.argv[1:2] == ['--diagnose']:
        if len(sys.argv) != 3:
            return 2
        from diagnostics import run
        return run(sys.argv[2])
    from audio_router_gui import main as gui_main
    return gui_main()


if __name__ == '__main__':
    raise SystemExit(main())

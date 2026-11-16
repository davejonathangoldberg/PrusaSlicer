#!/usr/bin/env python3
"""Capture raw Prusa printer USB output on macOS or Linux."""

import argparse
import os
import select
import stat
import sys
import termios
import tty


def capture(device, output_path):
    with open(output_path, 'xb') as output:
        fd = os.open(device, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            if not stat.S_ISCHR(os.fstat(fd).st_mode):
                raise OSError('Select a serial device')
            tty.setraw(fd, termios.TCSANOW)
            settings = termios.tcgetattr(fd)
            settings[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
            settings[2] &= ~(termios.HUPCL | getattr(termios, 'CRTSCTS', 0))
            settings[2] |= termios.CLOCAL | termios.CREAD
            settings[4] = settings[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, settings)
            print(f'Capturing from {device}. Press Ctrl-C to stop.', file=sys.stderr, flush=True)
            while True:
                select.select([fd], [], [])
                try:
                    data = os.read(fd, 8192)
                except BlockingIOError:
                    continue
                if not data:
                    raise OSError('Serial device disconnected')
                output.write(data)
                output.flush()
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('device', help='Serial device path, for example /dev/cu.usbmodem1234')
    parser.add_argument('output', help='Path for a new log file')
    args = parser.parse_args()
    try:
        capture(args.device, args.output)
    except KeyboardInterrupt:
        print(f'Capture stopped. Log saved to {args.output}.', file=sys.stderr)
        return 0
    except OSError as error:
        print(f'Capture failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

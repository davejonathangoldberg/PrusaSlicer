import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest


class CaptureTest(unittest.TestCase):
    def test_reads_unmodified_bytes_and_sends_no_serial_data(self):
        script = Path(__file__).resolve().parents[1] / 'build-utils' / 'capture_print_trace.py'
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / 'trace.log'
            with subprocess.Popen([sys.executable, str(script), os.ttyname(slave), str(output_path)],
                                  stderr=subprocess.PIPE) as recorder:
                try:
                    ready, _, _ = select.select([recorder.stderr], [], [], 5)
                    self.assertTrue(ready, 'The recorder did not start')
                    self.assertIn(b'Capturing', recorder.stderr.readline())
                    data = b'start\r\nTRACE,0,selected,1700\r\nTRACE,100,ram,1660\n'
                    os.write(master, data)
                    deadline = time.monotonic() + 5
                    while output_path.stat().st_size < len(data) and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertEqual(output_path.read_bytes(), data)
                    readable, _, _ = select.select([master], [], [], 0)
                    self.assertFalse(readable, 'The recorder sent data to the device')
                    recorder.send_signal(signal.SIGINT)
                    self.assertEqual(recorder.wait(timeout=5), 0)
                finally:
                    if recorder.poll() is None:
                        recorder.kill()


if __name__ == '__main__':
    unittest.main()

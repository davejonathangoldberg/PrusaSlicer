# Stock SD print trace

The **CPAM Trace** tab records normal G-code exports to local storage or an SD
card and displays USB logs from the MK3S trace firmware. The slicer and printer
record elapsed time on separate clocks. You can also open the tab from
**Window → CPAM Trace Tab**. The slicer starts its clock when you
confirm the export destination. The printer starts its clock when the LCD
accepts the SD file selection.

The tab keeps trace history in the slicer's local data directory. The slicer
saves each export trace when the export succeeds, fails, or cancels. To link a
printer attempt to an export, select the export and confirm the link during
import. Link one capture to each export. Import subsequent attempts as new
runs to keep the earlier record.

## Test on an MK3S or MK3S+

1. Build the slicer with the [macOS build instructions](How%20to%20build%20-%20Mac%20OS.md).
   Use an isolated `--datadir` for a test installation. Use the trace firmware
   build for the exact printer model.
2. Slice a small model. Use the normal SD export action and select a mounted SD
   card. Confirm Save. Check that **CPAM Trace** shows a new export and that the
   final G-code is on the card. Eject the card and insert it in the printer.
3. With the printer idle, close the firmware flash dialog and other serial
   applications. Start the recorder before you select the file on the LCD.
   Replace the port in this example with the printer's actual port.

   ```sh
   python3 build-utils/capture_print_trace.py /dev/cu.usbmodem1234 stock-print.log
   ```

   The recorder opens the port at 115200 baud and writes a new log. The USB
   connection can reset the printer when it opens. Wait for the printer to be
   ready before you use the LCD.
4. Select the same file on the printer LCD. Keep the capture open until the SD
   print finishes and the log contains `finished`. Stop the recorder with
   Ctrl-C. Select the export in the tab and use **Import printer log** to link the
   capture to it.
5. Check that the stock file-check markers appear in order and that `finished`
   follows the real print completion. Use the memory table to check the actual
   sample intervals during startup and printing. The target interval is 100 ms.
   A gap or a loss record indicates missing data. Close and reopen the slicer
   and confirm that the run still appears in history.
6. Repeat with a new capture and cancel the print after it starts. Import this
   capture as a new run. Confirm that the earlier successful run stays in
   history and the new run has a cancellation record.

Run the recorder check on macOS or Linux with
`python3 tests/print_trace_capture_test.py`. It uses a local pseudo-terminal and
checks that the recorder preserves the input bytes and sends no serial data.

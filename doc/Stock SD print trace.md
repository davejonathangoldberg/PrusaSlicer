# Stock SD print trace

Open **CPAM Trace** in PrusaSlicer or select **Window → CPAM Trace Tab**.
The tab records G-code exports to local storage and SD cards. The slicer starts
its clock when you confirm Save. The printer uses a separate clock that starts
when the LCD accepts the SD file selection.

## Capture a print over USB

Live USB capture is available on macOS and Linux.

1. Use the trace firmware for the exact MK3S or MK3S+ model. Slice a small
   model and export it to a mounted SD card. Confirm that the tab shows the export
   and that the G-code is on the card. Eject the card and insert it in the printer.
2. With the printer idle, close the firmware flash dialog and other serial
   applications. In **CPAM Trace**, select the printer's USB port and click
   **Connect printer**. The slicer reads the port at 115200 baud. Opening the
   port can reset the printer. Wait for the LCD to show that the printer is
   ready before you start the print.
3. To link the next print to an export, select that saved export and click
   **Track next print**. This applies to one LCD selection. For a separate
   history entry, start the print without selecting **Track next print**.
4. Select the SD file on the printer LCD. The first records appear after the
   first G-code command or an early stop. The time values come from the printer.
   The charts and tables update as the rest of the data arrives.
5. Check that the stock file-check markers appear in order and that
   **Print complete** appears after the SD print finishes. The free SRAM table
   shows the actual sample intervals. The target is 100 ms. The tab reports
   missing samples or events when the firmware reports a loss. After the print,
   restart the slicer and confirm that the run is still in history.
6. Repeat with a new print and cancel it after it starts. Confirm that the
   earlier run is still in history and that the new run has a cancellation
   record. Disconnect in the tab before you open the firmware flash dialog
   or use another application on the same port.

The tab keeps captured data in the slicer's local data directory. If the
connection stops during a print, the tab marks the capture as interrupted
and keeps the records already received. Connect again while the printer is idle.

## Open a saved USB capture

Use **Import printer log** to inspect a capture from another session. Select a
saved export first if you want to link one print from the log to it. Import
subsequent prints as new runs to keep the earlier record.

The standalone recorder is available on macOS and Linux. Start it before LCD
selection and stop it with Ctrl-C after the final records arrive. Replace the
example port with the printer's port.

```sh
python3 build-utils/capture_print_trace.py /dev/cu.usbmodem1234 stock-print.log
```

For a new build, see the [macOS build instructions](How%20to%20build%20-%20Mac%20OS.md).
Use a separate `--datadir` when you test the slicer.

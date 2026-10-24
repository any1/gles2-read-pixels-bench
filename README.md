# glReadPixels benchmark

For the full test report see: [REPORT.md](REPORT.md)

To run this on your own device:

```
git clone https://github.com/any1/gles2-read-pixels-bench.git
cd gles2-read-pixels-bench
meson setup build
meson compile -C build
./build/gles2-read-pixels-bench | ./scripts/fps-graph.py >reports/YOUR_DEVICE_NAME.json
```

If you wish to share your results, commit your report json file and regenerate
the report:

```
./scripts/md-report.py
```

This will update `REPORT.md`. Then create a pull-request.

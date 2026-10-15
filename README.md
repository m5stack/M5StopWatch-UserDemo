# M5StopWatch-UserDemo
M5Stack StopWatch user demo for hardware evaluation.

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

## FluidBox Integration
With genuine thanks to [omiya-bonsai](https://github.com/omiya-bonsai) for the open-source "FluidBox" fluid simulation demo for the M5Stack Stopwatch.
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon)

## Power LED Off while Display On
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon) the green power indicator LED will now switch off when the AMOLED display is on. This will save a small amount of power, but also eliminate the distracting green LED when it is not necessary.

## PlatformIO/PIOArduino build support
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon) you can now load the repository folder in Visual Studio Code with either PlatformIO or PIOArduino installed, and build/upload the demo directly to your M5Stack StopWatch device.
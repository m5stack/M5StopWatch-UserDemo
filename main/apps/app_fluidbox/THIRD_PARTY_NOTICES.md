# Integration baseline

This M5StopWatch-UserDemo integration imported the FluidBox simulation from
`Flowduino/STOPWATCH-FluidBox` main at commit
`1770f4b8766f8a3fea0cb1fe3eb26eecc5ba5a00`.

The launcher integration replaces the standalone Arduino/M5Unified lifecycle
with the factory demo's ESP-IDF/Mooncake/HAL lifecycle while retaining the
FluidBox solver and M5GFX rendering model.

# Third-party notices

The FluidBox simulation and rendering design in `FluidBox.cpp` is adapted from
the ideas and substantial algorithmic structure of:

- V4C38/esp32-fluidbox: https://github.com/V4C38/esp32-fluidbox

Waveshare-specific display, GPIO, IMU, power-management, and ESP-IDF startup
code is not included in this project.

## V4C38/esp32-fluidbox license

MIT License

Copyright (c) 2026 V4C38

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

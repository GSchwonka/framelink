# framelink on ESP32

An ESP-IDF project with four small apps that run framelink on a real ESP32 and produce the hardware numbers in [docs/MEASUREMENTS.md](../../docs/MEASUREMENTS.md).

Tested on an **ESP32-WROOM-32D** dev board (dual-core Xtensa LX6 at 240 MHz). Any board with a classic ESP32 works. CI builds every app with ESP-IDF 5.5 and 6.1.

| App | Build config | Wiring | What it does |
|-----|--------------|--------|--------------|
| Echo over USB | `sdkconfig.echo_uart` | none | framelink on the USB serial port. Every frame comes back. Used by the Python host for interop and recovery tests |
| Loopback | `sdkconfig.loopback` | 2 wires | UART1 and UART2 on the same chip talk to each other. Measures throughput at 115200 and 921600 baud, with and without flow control |
| Echo over Wi-Fi | `sdkconfig.echo_tcp` | none | The same echo peer over TCP |
| Benchmark | `sdkconfig.bench`, `sdkconfig.bench_table` | none | Decoder and encoder cost in CPU cycles, including the decoder's worst case |

## 1. Install ESP-IDF

Follow Espressif's [getting started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/). On Windows, the ESP-IDF installer sets everything up and adds an "ESP-IDF CMD" shortcut. Run all the commands below from that shell.

## 2. Build and flash

Pick an app by listing its config file after `sdkconfig.defaults`. For example, the benchmark:

```sh
cd ports/esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" build
idf.py -p COM5 flash
```

Replace `COM5` with your port: `COMx` on Windows, `/dev/ttyUSB0` on Linux. Device Manager or `idf.py -p ? flash` lists them.

**Switching apps:** ESP-IDF keeps the last configuration in a file called `sdkconfig`. Delete it before building a different app, or the old settings win:

```sh
del sdkconfig        # Windows
rm sdkconfig         # Linux and macOS
```

## 3. Run each app

### Benchmark, no wiring

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" build
idf.py -p COM5 flash
python ../../bench/esp32_collect.py --port COM5 --jsonl ../../bench/results/esp32_bench_bitwise.jsonl
```

Then repeat with `sdkconfig.bench_table` and `esp32_bench_table.jsonl` to measure the table-driven CRC. The script resets the board, reads the results and prints them as tables. It takes about 10 seconds.

### Loopback, two wires

Unplug the USB cable first. Connect:

| From | To |
|------|----|
| **GPIO25** (UART1 TX) | **GPIO16** (UART2 RX) |
| **GPIO17** (UART2 TX) | **GPIO26** (UART1 RX) |

Pins are printed on the board next to each header pin. Some boards label them `25`, `16`, `17`, `26`, some `D25` or `IO25`.

**Using strands from an Ethernet cable.** These work well. They are solid copper, about 0.5 mm thick.
- Strip about 1 cm of insulation from each end.
- Wrap each end tightly around its pin, at least three turns, or push it into a breadboard next to the pin.
- Make sure no bare copper touches a neighbouring pin. A wire touching **3V3, 5V or GND** together with a GPIO can damage the board, so check twice before plugging the USB cable back in.

Then run:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.loopback" build
idf.py -p COM5 flash
python ../../bench/esp32_collect.py --port COM5 --jsonl ../../bench/results/esp32_loopback.jsonl
```

It runs six tests of 10 seconds each, so it takes about a minute. If it prints `FLERROR no handshake`, a wire is loose or on the wrong pin.

To use other pins, change them in `idf.py menuconfig`, under framelink demo, then Loopback pins. Avoid GPIO6 to GPIO11, which are wired to the flash chip, and GPIO34 to GPIO39, which are input only.

### Echo over USB, for the Python host

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.echo_uart" build
idf.py -p COM5 flash
```

This app turns off the ESP32 console, so `idf.py monitor` shows nothing useful. Talk to it with the Python host instead, from the repository root:

```sh
pip install -e "host[serial]"
framelink bench serial:COM5@921600 --count 1000
python bench/host_recovery.py --device serial:COM5@921600 --csv bench/results/esp32_uart_recovery.csv
```

Close any serial monitor first. Only one program can open the port at a time.

### Echo over Wi-Fi

Set your network name and password in `idf.py menuconfig`, under framelink demo, then Wi-Fi. They stay in your local `sdkconfig` and are never committed. Then:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.echo_tcp" build
idf.py -p COM5 flash monitor
```

The console prints the board's IP address. From another terminal:

```sh
framelink bench tcp:192.168.1.50:5000 --count 1000
python bench/host_recovery.py --device tcp:192.168.1.50:5000
```

## How the port works

The whole integration is in [main/common.c](main/common.c):

- `fl_uart_write` and `fl_uart_read` adapt the ESP-IDF UART driver to framelink's two callbacks. Reading never blocks.
- A FreeRTOS task calls `fl_poll` once per millisecond with `esp_timer_get_time() / 1000`, which is the only clock framelink needs.
- Decoding happens in that task, never in an interrupt. The ESP-IDF UART driver's interrupt only fills a 4 KB ring buffer.
- The library itself is used unchanged, straight from `framelink/` at the repository root. See [components/framelink](components/framelink/CMakeLists.txt).

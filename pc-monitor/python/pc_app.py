import asyncio

import psutil
from bleak import BleakClient
from dbus_fast import BusType, Message, MessageType
from dbus_fast.aio import MessageBus

try:
    import pynvml

    pynvml.nvmlInit()
    HAS_NVIDIA = True
except Exception as e:
    print(f"[WARN] Khong dung duoc NVML: {e!r}")
    HAS_NVIDIA = False

ESP_ADDRESS = "C0:4E:30:29:21:7D"
CHAR_UUID = "6e5f0002-b5a3-f393-e0a9-e50e24dcca9e"
ADAPTER = "hci0"

SEND_INTERVAL = 5.0
GB = 1024 ** 3

DEV_PATH = f"/org/bluez/{ADAPTER}/dev_" + ESP_ADDRESS.replace(":", "_")


# ---------------------------------------------------------------- thu thap

def get_cpu_temp():
    try:
        temps = psutil.sensors_temperatures()
        for key in ("coretemp", "k10temp", "cpu_thermal"):
            if key in temps and temps[key]:
                return temps[key][0].current
        for entries in temps.values():
            if entries:
                return entries[0].current
    except Exception as e:
        print(f"[WARN] Doc nhiet do CPU loi: {e!r}")
    return 0.0


def get_gpu_stats():
    if not HAS_NVIDIA:
        return None
    try:
        handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        mem = pynvml.nvmlDeviceGetMemoryInfo(handle)
        try:
            power = pynvml.nvmlDeviceGetPowerUsage(handle) / 1000.0
        except pynvml.NVMLError:
            power = 0.0
        return {
            "util": pynvml.nvmlDeviceGetUtilizationRates(handle).gpu,
            "temp": pynvml.nvmlDeviceGetTemperature(handle, pynvml.NVML_TEMPERATURE_GPU),
            "power": power,
            "vram_used": mem.used / GB,
            "vram_total": mem.total / GB,
        }
    except Exception as e:
        print(f"[WARN] Doc GPU loi: {e!r}")
        return None


def collect():
    mem = psutil.virtual_memory()
    return {
        # interval=None -> khong block; tra ve trung binh ke tu lan goi truoc,
        # tuc dung bang chu ky 5s cua vong lap
        "cpu": psutil.cpu_percent(interval=None),
        "cpu_temp": get_cpu_temp(),
        "ram_used": mem.used / GB,
        "ram_total": mem.total / GB,
        "gpu": get_gpu_stats(),
    }


def to_wire(d):
    """key=value ngan cach bang dau ; -- ESP tu quyet dinh hien thi the nao."""
    parts = [
        f"cpu={d['cpu']:.0f}",
        f"ct={d['cpu_temp']:.0f}",
        f"ram={d['ram_used']:.1f}",
        f"ramt={d['ram_total']:.0f}",
    ]
    g = d["gpu"]
    if g:
        parts += [
            f"gpu={g['util']}",
            f"gt={g['temp']}",
            f"gw={g['power']:.0f}",
            f"vr={g['vram_used']:.1f}",
            f"vrt={g['vram_total']:.0f}",
        ]
    return ";".join(parts)


def to_human(d):
    line = (f"CPU:{d['cpu']:.0f}% {d['cpu_temp']:.0f}C | "
            f"RAM:{d['ram_used']:.1f}/{d['ram_total']:.0f}G")
    g = d["gpu"]
    if g:
        line += (f" | GPU:{g['util']}% {g['temp']}C {g['power']:.0f}W "
                 f"VRAM:{g['vram_used']:.1f}/{g['vram_total']:.0f}G")
    return line


# ------------------------------------------------------------- theo doi BlueZ

class ConnectionWatcher:
    """Theo doi thuoc tinh Connected cua thiet bi trong cay D-Bus cua BlueZ.

    Script KHONG bao gio tu goi Connect(). Bat/tat hoan toan do toggle trong
    Settings quyet dinh -- giong het cach mot con chuot Bluetooth hoat dong.
    """

    def __init__(self):
        self.connected = asyncio.Event()
        self._bus = None

    async def start(self):
        self._bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

        # Nghe PropertiesChanged cua rieng object path thiet bi cua ta
        await self._bus.call(Message(
            destination="org.freedesktop.DBus",
            path="/org/freedesktop/DBus",
            interface="org.freedesktop.DBus",
            member="AddMatch",
            signature="s",
            body=[
                "type='signal',"
                "interface='org.freedesktop.DBus.Properties',"
                "member='PropertiesChanged',"
                f"path='{DEV_PATH}'"
            ],
        ))
        self._bus.add_message_handler(self._on_signal)

        if await self._read_connected():
            self.connected.set()

        state = "da ket noi" if self.connected.is_set() else "chua ket noi"
        print(f"[BLUEZ] Dang theo doi {DEV_PATH} ({state})")

    async def _read_connected(self):
        try:
            reply = await self._bus.call(Message(
                destination="org.bluez",
                path=DEV_PATH,
                interface="org.freedesktop.DBus.Properties",
                member="Get",
                signature="ss",
                body=["org.bluez.Device1", "Connected"],
            ))
            if reply.message_type == MessageType.METHOD_RETURN:
                return bool(reply.body[0].value)
        except Exception as e:
            print(f"[BLUEZ] Chua doc duoc trang thai: {e!r}")
        return False

    def _on_signal(self, msg):
        if msg.member != "PropertiesChanged" or msg.path != DEV_PATH:
            return
        iface, changed, _ = msg.body
        if iface != "org.bluez.Device1" or "Connected" not in changed:
            return

        if changed["Connected"].value:
            print("[BLUEZ] Toggle BAT -> bat dau gui")
            self.connected.set()
        else:
            print("[BLUEZ] Toggle TAT -> ngung gui")
            self.connected.clear()


# ------------------------------------------------------------------- gui

async def send_loop(watcher):
    """Chay khi dang ket noi. Thoat ngay khi toggle bi tat."""
    async with BleakClient(ESP_ADDRESS) as client:
        print("-> Da mo GATT, bat dau gui\n")
        psutil.cpu_percent(interval=None)  # lan doc dau tien luon tra ve 0.0

        while watcher.connected.is_set():
            data = collect()
            await client.write_gatt_char(CHAR_UUID, to_wire(data).encode(), response=True)
            print(f"[PC Sent]: {to_human(data)}")

            # Cho 5s, nhung bung ra ngay neu toggle bi tat giua chung
            try:
                await asyncio.wait_for(_wait_cleared(watcher), timeout=SEND_INTERVAL)
                return
            except asyncio.TimeoutError:
                pass


async def _wait_cleared(watcher):
    while watcher.connected.is_set():
        await asyncio.sleep(0.2)


async def main():
    watcher = ConnectionWatcher()
    await watcher.start()

    while True:
        await watcher.connected.wait()
        try:
            await send_loop(watcher)
        except Exception as e:
            print(f"[ERR] {e!r}")
            await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nDa dung chuong trinh!")
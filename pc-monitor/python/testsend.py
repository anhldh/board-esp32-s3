# test_send.py
import asyncio
from bleak import BleakClient

ADDR = "C0:4E:30:29:21:7D"
CHAR = "6e5f0002-b5a3-f393-e0a9-e50e24dcca9e"

async def main():
    async with BleakClient(ADDR) as c:
        await c.write_gatt_char(
            CHAR, b"cpu=50;ct=60;ram=8.0;ramt=32", response=True
        )
        print("Da gui")
        await asyncio.sleep(2)

asyncio.run(main())
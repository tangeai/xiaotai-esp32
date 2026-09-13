#!/usr/bin/env python3
"""Opt-in serial capture of pre-SDK H264. Close idf.py monitor before use."""
import argparse
import re
import shutil
import subprocess
import time
from pathlib import Path


class Terminal:
    """Minimal ANSI responder; retain partial lines across serial read timeouts."""
    def __init__(self, port):
        self.port = port
        self.pending = bytearray()
        self.tail = bytearray()
        self.received = 0
        self.signals = []

    def write(self, data):
        return self.port.write(data)

    def readline(self):
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            if b"\n" in self.pending:
                line, _, rest = self.pending.partition(b"\n")
                self.pending = bytearray(rest)
                for signature in (b"Guru Meditation", b"panic'ed", b"assert failed",
                                  b"CORRUPT HEAP", b"Stack smashing", b"Brownout",
                                  b"Rebooting", b"ESP-ROM:"):
                    if signature in line and signature.decode() not in self.signals:
                        self.signals.append(signature.decode())
                        # Do not print unrelated runtime logs or credentials.
                        print(f"设备诊断标记：{signature.decode()}", flush=True)
                return bytes(line) + b"\n"
            try:
                chunk = self.port.read(min(self.port.in_waiting or 1, 4096))
            except OSError as error:
                raise RuntimeError(
                    f"串口读取中断（已收到{self.received}字节；"
                    f"已观测异常标记={','.join(self.signals) or '无，不能排除重启'}）。"
                    "请检查串口占用及USB断连/设备重启；未自动重连或清除设备抓流。"
                ) from error
            self.received += len(chunk)
            for byte in chunk:
                self.tail.append(byte)
                self.tail = self.tail[-8:]
                if self.tail.endswith(b"\x1b[6n"):
                    self.write(b"\x1b[1;80R")
                elif self.tail.endswith(b"\x1b[5n"):
                    self.write(b"\x1b[0n")
            self.pending.extend(chunk)
            if len(self.pending) > 16384 and b"\n" not in self.pending:
                raise RuntimeError("console line too long; check serial port/baud")
        return b""

    def settle(self):
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            self.readline()

    def command(self, text):
        self.settle()
        # Ctrl-U clears a stale partially typed command; REPL expects CR.
        self.write(b"\x15" + text.encode("ascii") + b"\r")


def firmware_identity(port):
    port.settle()
    port.command("version")
    try:
        return until(port, "Firmware:", timeout=5)
    except TimeoutError:
        # An earlier monitor may have consumed the cursor query without replying.
        # Complete that outstanding read, then clear the input and retry once.
        port.write(b"\x1b[1;80R")
        port.settle()
        port.command("version")
        return until(port, "Firmware:", timeout=10)


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


class Dump:
    def __init__(self):
        self.data = bytearray()
        self.meta = None

    def feed(self, line):
        # Ignore unrelated logs, but reject holes, duplicates and corrupted data.
        start = re.search(r"VCAP BEGIN bytes=(\d+) fnv=([0-9a-f]{8}) mode=(\d+) "
                          r"generation=(\d+) width=(\d+) height=(\d+) frames=(\d+)", line)
        if start:
            if self.meta is not None:
                raise ValueError("duplicate dump header")
            self.meta = [int(start[1]), int(start[2], 16)] + [int(start[i]) for i in range(3, 8)]
            if not 0 < self.meta[0] <= 512 * 1024:
                raise ValueError("invalid dump size")
        elif "VCAP DATA " in line:
            match = re.search(r"VCAP DATA ([0-9a-f]{8}) ([0-9a-f]+)\s*$", line)
            if self.meta is None or not match or int(match[1], 16) != len(self.data):
                raise ValueError("missing/corrupted/out-of-order dump line; retry dump")
            chunk = bytes.fromhex(match[2])
            if not 0 < len(chunk) <= 64 or len(self.data) + len(chunk) > self.meta[0]:
                raise ValueError("invalid chunk size")
            self.data.extend(chunk)
        elif "VCAP END" in line:
            if self.meta is None or len(self.data) != self.meta[0] or fnv(self.data) != self.meta[1]:
                raise ValueError("incomplete dump or checksum mismatch; retry dump")
            return True
        elif "VCAP ERROR" in line:
            raise RuntimeError(line[line.index("VCAP ERROR"):].strip())
        return False


def until(port, marker, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode("ascii", errors="replace")
        if "VCAP ERROR" in line:
            raise RuntimeError(line[line.index("VCAP ERROR"):].strip())
        if marker in line:
            return line[line.index(marker):].strip()
    raise TimeoutError(f"did not receive {marker}; check port/firmware")


def capture_status(port, command="status"):
    port.command("video-capture " + command)
    line = until(port, "VCAP STATUS")
    print(line, flush=True)
    match = re.search(r"active=(\d+) bytes=(\d+) frames=(\d+) width=(\d+) height=(\d+) reason=(\S+)", line)
    if not match:
        raise RuntimeError("抓流状态不完整，请保留上述状态输出")
    return dict(zip(("active", "bytes", "frames", "width", "height", "reason"),
                    [int(match[i]) for i in range(1, 6)] + [match[6]]))


def stop_for_export(port):
    status = capture_status(port, "stop")
    if status["active"]:
        raise RuntimeError("设备尚未停止抓流，未尝试导出")
    if not status["bytes"]:
        # No media to preserve. Release the allocation that otherwise blocks start.
        capture_status(port, "clear")
        raise RuntimeError(
            f"缓冲为0字节，没有可导出的H264（reason={status['reason']}）；"
            "已释放空缓冲。请保留上面的STATUS输出，重新建立视频后运行不带--dump-only的命令。"
        )
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--dump-only", action="store_true", help="retry exporting retained capture after hangup")
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".png").exists() or args.output.with_suffix(".txt").exists():
        parser.error("output or companion file exists; choose a new name")
    import serial
    # POSIX advisory exclusive access: prevents cooperating serial tools from
    # taking the same port; cannot evict/detect every already-open reader.
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=5, exclusive=True) as raw_port:
        port = Terminal(raw_port)
        if not args.dump_only:
            input("保持标识纸不动，进入 H5 实时查看或微信视频通话，出图后按 Enter：")
            identity = firmware_identity(port)
            print(identity)
            status = capture_status(port)
            if status["active"] or status["bytes"]:
                raise RuntimeError("存在正在进行或已保存的抓流，未覆盖。请退出查看后用--dump-only导出")
            if status["reason"] != "empty":
                capture_status(port, "clear")  # inactive, zero-byte allocation only
            port.command("video-capture start")
            print(until(port, "VCAP ARMED"))
            # Drain unrelated logs while allowing up to 5s for IDR + 2s capture.
            deadline = time.monotonic() + 7
            while time.monotonic() < deadline:
                port.readline()
        else:
            identity = firmware_identity(port)
            print(identity)
        stop_for_export(port)
        input("现在退出所有 H5 查看页面/挂断通话；确认连接断开后按 Enter 导出（不要只最小化页面）：")
        port.command("video-capture dump")
        dump = Dump()
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            if dump.feed(port.readline().decode("ascii", errors="replace")):
                break
        else:
            raise TimeoutError("dump timed out; retained on device, retry with --dump-only")
        with args.output.open("xb") as file:
            file.write(dump.data)
        with args.output.with_suffix(".txt").open("x") as file:
            file.write(identity + "\n")
            file.write(f"bytes,fnv,mode,generation,width,height,frames={dump.meta}\n")
        port.command("video-capture clear")
        until(port, "VCAP STATUS")
    print(f"校验通过，已保存 {args.output}；设备抓流缓冲已释放。")
    if shutil.which("ffprobe"):
        subprocess.run(["ffprobe", "-v", "error", "-f", "h264", "-show_entries",
                        "stream=codec_name,width,height", "-of", "default=nw=1", str(args.output)], check=True)
    if shutil.which("ffmpeg"):
        subprocess.run(["ffmpeg", "-v", "error", "-n", "-noautorotate", "-f", "h264", "-i",
                        str(args.output), "-frames:v", "1", str(args.output.with_suffix(".png"))], check=True)
        print(f"发送前画面（未自动旋转）：{args.output.with_suffix('.png')}")


if __name__ == "__main__":
    try:
        main()
    except (TimeoutError, RuntimeError, OSError, ValueError) as error:
        raise SystemExit(f"抓流未完成：{error}") from None

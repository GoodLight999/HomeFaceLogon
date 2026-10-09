"""Exercise the built setup helper with synthetic data in an isolated directory."""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct
import subprocess
import tempfile


class Blob(ctypes.Structure):
    _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_ubyte))]


def decrypt_secret(path):
    data = path.read_bytes()
    magic, version, flags, length = struct.unpack_from("<IHHI", data)
    assert (magic, version, flags) == (0x4F4C4648, 1, 0)
    assert length == len(data) - 12
    buffer = (ctypes.c_ubyte * length).from_buffer_copy(data[12:])
    source = Blob(length, buffer)
    output = Blob()
    crypt = ctypes.WinDLL("crypt32", use_last_error=True)
    crypt.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    crypt.CryptUnprotectData.restype = wintypes.BOOL
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    assert crypt.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output))
    try:
        return ctypes.string_at(output.pbData, output.cbData).decode("utf-16-le").rstrip("\0")
    finally:
        ctypes.memset(output.pbData, 0, output.cbData)
        kernel.LocalFree(output.pbData)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=Path(__file__).resolve().parents[1] / "x64/Release/FaceLogonSetup.exe")
    args = parser.parse_args()
    exe = args.exe.resolve()
    capabilities = subprocess.run([str(exe), "--capabilities"], capture_output=True, timeout=15)
    assert json.loads(capabilities.stdout) == {"protocolVersion": 1, "isolatedDataDirectory": True}
    with tempfile.TemporaryDirectory(prefix="hfl-setup-regression-") as workspace:
        root = Path(workspace)
        candidate = root / "candidate"
        candidate.mkdir()
        password = "Dummy-テスト🔑-пароль-é!"
        result = subprocess.run([str(exe), "--data-dir", str(candidate), "--sid", "S-1-5-21-123-456-789-1001"],
            input=(password + "\n1234\n").encode("utf-8"), capture_output=True, timeout=30)
        assert result.returncode == 0, "Synthetic credential setup failed"
        assert decrypt_secret(candidate / "secret.bin") == password, "Password encoding changed"
        config = json.loads((candidate / "config.json").read_text(encoding="utf-8"))
        assert config["targetSid"] == "S-1-5-21-123-456-789-1001"
        assert (candidate / "pin.bin").stat().st_size == 60
        print("PASS setup helper preserves Japanese, emoji and other UTF-8 password characters")

        snapshot = {name: (candidate / name).read_bytes() for name in ["secret.bin", "pin.bin", "config.json"]}
        for invalid in [b"bad\n12\n", b"\xff\n1234\n", b"bad\x00input\n1234\n"]:
            result = subprocess.run([str(exe), "--data-dir", str(candidate), "--sid", "S-1-5-21-123-456-789-1001"],
                input=invalid, capture_output=True, timeout=30)
            assert result.returncode != 0, "Invalid input was accepted"
            assert all((candidate / name).read_bytes() == data for name, data in snapshot.items())
        print("PASS invalid PIN, invalid UTF-8 and NUL input leave synthetic credentials unchanged")

        face = candidate / "face.bin"
        face.write_bytes(b"synthetic-face-sentinel")
        snapshot["face.bin"] = face.read_bytes()
        result = subprocess.run([str(exe), "--data-dir", str(candidate), "--test"],
            capture_output=True, timeout=60)
        assert all((candidate / name).read_bytes() == data for name, data in snapshot.items())
        assert not list(candidate.glob("*.bak"))
        print("PASS diagnostics never alter enrolled data (diagnostics result: " + str(result.returncode) + ")")
        assert result.returncode == 0, "Model/DPAPI diagnostics failed"


if __name__ == "__main__":
    main()

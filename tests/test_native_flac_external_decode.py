#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SKIP_RETURN_CODE = 77


def skip(message):
    print(f"SKIP: {message}")
    return SKIP_RETURN_CODE


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pack_group(samples):
    words = [(sample // 64) + 512 for sample in samples]
    return bytes(
        [
            (words[0] & 0x03FC) >> 2,
            ((words[0] & 0x0003) << 6) + ((words[1] & 0x03F0) >> 4),
            ((words[1] & 0x000F) << 4) + ((words[2] & 0x03C0) >> 6),
            ((words[2] & 0x003F) << 2) + ((words[3] & 0x0300) >> 8),
            words[3] & 0x00FF,
        ]
    )


def make_fixture(group_count=2304):
    lds = bytearray()
    pcm = bytearray()
    for group in range(group_count):
        samples = [((((group * 7) + index) % 1024) - 512) * 64 for index in range(4)]
        lds.extend(pack_group(samples))
        for sample in samples:
            pcm.extend(struct.pack("<h", sample))
    return bytes(lds), bytes(pcm)


def run_checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    return result


def run_binary_decode(args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(
            f"decode command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result.stdout


def resolve_tool(path_or_name):
    if not path_or_name:
        return None
    if path_or_name.endswith("-NOTFOUND"):
        return None
    path = Path(path_or_name)
    if path.exists():
        return str(path)
    resolved = shutil.which(path_or_name)
    return resolved


def ffprobe_native_flac(ffprobe, flac_path):
    if ffprobe is None:
        return
    result = run_checked(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=codec_name,sample_rate,channels",
            "-of",
            "json",
            str(flac_path),
        ]
    )
    streams = json.loads(result.stdout).get("streams", [])
    require(streams, "ffprobe did not find an audio stream")
    stream = streams[0]
    require(stream.get("codec_name") == "flac", "ffprobe did not report FLAC audio")
    require(stream.get("sample_rate") == "40000", "ffprobe did not report 40 kHz sample rate")
    require(stream.get("channels") == 1, "ffprobe did not report mono audio")


def compress_native_fixed(binary, lds_path, flac_path):
    run_checked(
        [
            str(binary),
            "compress",
            "--backend",
            "native-fixed",
            "--overwrite",
            str(lds_path),
            str(flac_path),
        ]
    )
    require(flac_path.exists(), "native-fixed compression did not create output")


def compress_cpu_ogg(binary, lds_path, flac_path):
    run_checked(
        [
            str(binary),
            "compress",
            "--backend",
            "cpu",
            "--overwrite",
            str(lds_path),
            str(flac_path),
        ]
    )
    require(flac_path.exists(), "CPU compression did not create output")


def decode_with_ffmpeg(args, flac_path):
    ffmpeg = resolve_tool(args.ffmpeg)
    if ffmpeg is None:
        return None, "ffmpeg is not available"
    ffprobe = resolve_tool(args.ffprobe)
    ffprobe_native_flac(ffprobe, flac_path)
    decoded = run_binary_decode(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(flac_path),
            "-map",
            "0:a:0",
            "-c:a",
            "pcm_s16le",
            "-f",
            "s16le",
            "-",
        ]
    )
    return decoded, None


def decode_with_ld_decode_pyav(args, flac_path):
    ldf_reader = Path(args.ldf_reader) if args.ldf_reader else None
    if ldf_reader is None or not ldf_reader.exists():
        return None, f"reference ld-decode reader not found: {ldf_reader}"

    try:
        import av  # noqa: F401
    except ImportError:
        return None, "PyAV is not installed"

    decoded = run_binary_decode(
        [sys.executable, str(ldf_reader), "-q", str(flac_path)]
    )
    return decoded, None


def close_loader(loader):
    close = getattr(loader, "_close", None)
    if close is not None:
        close()


def require_loader_bytes(utils, path, sample, readlen, expected_pcm):
    loader = utils.make_loader(str(path))
    try:
        require(
            isinstance(loader, utils.LoadLDF),
            f"{path.name} did not dispatch to LoadLDF",
        )
        with path.open("rb") as infile:
            decoded = loader(infile, sample, readlen)
        require(decoded is not None, f"{path.name} loader returned EOF")
        expected = expected_pcm[sample * 2 : (sample + readlen) * 2]
        require(
            decoded.tobytes() == expected,
            f"{path.name} loader changed samples at offset {sample}",
        )
    finally:
        close_loader(loader)


def decode_with_ld_decode_loader(args, binary, lds_path, temp_dir, expected_pcm):
    ld_decode_dir = Path(args.ld_decode_dir) if args.ld_decode_dir else None
    if ld_decode_dir is None or not ld_decode_dir.exists():
        return "reference ld-decode directory not found"

    os.environ["NUMBA_CACHE_DIR"] = str(temp_dir / "numba-cache")
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ld_decode_dir))
    try:
        from lddecode import utils
    except ImportError as ex:
        return f"reference ld-decode loader dependencies are not available: {ex}"

    ogg_ldf = temp_dir / "fixture.ldf"
    ogg_raw_oga = temp_dir / "fixture.raw.oga"
    native_flac_ldf = temp_dir / "fixture.flac.ldf"
    native_flac = temp_dir / "fixture.flac"
    compress_cpu_ogg(binary, lds_path, ogg_ldf)
    shutil.copyfile(ogg_ldf, ogg_raw_oga)
    compress_native_fixed(binary, lds_path, native_flac_ldf)
    shutil.copyfile(native_flac_ldf, native_flac)

    outputs = [
        ("ogg .ldf", ogg_ldf),
        ("ogg .raw.oga", ogg_raw_oga),
        ("native .flac.ldf", native_flac_ldf),
        ("native .flac", native_flac),
    ]

    total_samples = len(expected_pcm) // 2
    offset_sample = 257
    offset_readlen = 4096
    require(
        offset_sample + offset_readlen <= total_samples,
        "fixture is too small for nonzero loader read",
    )
    for _, path in outputs:
        require_loader_bytes(utils, path, 0, total_samples, expected_pcm)
        require_loader_bytes(utils, path, offset_sample, offset_readlen, expected_pcm)

    return None


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--decoder",
        choices=("ffmpeg", "ld-decode-pyav", "ld-decode-loader"),
        required=True,
    )
    parser.add_argument("--binary", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--ld-decode-dir")
    parser.add_argument("--ldf-reader")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    binary = Path(args.binary)
    require(binary.exists(), f"ld-compress-ng not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="ld-compress-ng-external-decode-") as temp:
        temp_dir = Path(temp)
        lds_path = temp_dir / "fixture.lds"
        flac_path = temp_dir / "fixture.flac.ldf"
        expected_lds, expected_pcm = make_fixture()
        lds_path.write_bytes(expected_lds)

        if args.decoder == "ffmpeg":
            compress_native_fixed(binary, lds_path, flac_path)
            decoded, skip_reason = decode_with_ffmpeg(args, flac_path)
            if skip_reason is None:
                require(
                    decoded == expected_pcm,
                    f"{args.decoder} changed native .flac.ldf PCM bytes",
                )
        elif args.decoder == "ld-decode-pyav":
            compress_native_fixed(binary, lds_path, flac_path)
            decoded, skip_reason = decode_with_ld_decode_pyav(args, flac_path)
            if skip_reason is None:
                require(
                    decoded == expected_pcm,
                    f"{args.decoder} changed native .flac.ldf PCM bytes",
                )
        else:
            skip_reason = decode_with_ld_decode_loader(
                args, binary, lds_path, temp_dir, expected_pcm)

        if skip_reason is not None:
            return skip(skip_reason)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as ex:
        print(f"test_native_flac_external_decode: {ex}", file=sys.stderr)
        sys.exit(1)

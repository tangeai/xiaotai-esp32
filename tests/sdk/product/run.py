#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and run generated products offline on a POSIX host with CTest 3.21+."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
EXCLUSIVE_PROVISION_TESTS = ("environment_provision_idle", "environment_provision_verifying",
                             "environment_provision_timeout")
PLATFORM_BINDING_TESTS = ("environment_binding_online", "environment_binding_verify",
                          "environment_binding_confirmed", "environment_binding_rejected")
S3_DISCOVERY_TESTS = tuple(f"endpoint_{case}" for case in (
    "startup_order", "path", "ipv6", "long", "missing", "redirect", "mandatory", "recognition",
    "invalid_recognition", "retry_missing", "retry_invalid", *(f"bad{i}" for i in range(10)),
))
S3_PROFILE_MODEL_TESTS = tuple(f"profile_video_model_{case}" for case in (
    "null", "empty", "quote", "backslash", "newline", "space", "forward_slash", "bytes63", "bytes64",
))
SERVICE_POLICY_TESTS = (
    *(f"endpoint_policy_http_bad_{case}" for case in range(12)),
    *(f"endpoint_policy_mqtt_bad_{case}" for case in range(17)),
    *(f"endpoint_policy_mqtt_good_{case}" for case in range(7)),
    "endpoint_policy_missing_rtc", "endpoint_policy_retry_201",
)
S3_STARTUP_TESTS = ("stack_business", "history_maintenance", "clock_gate_bound", "clock_gate_binding",
                    "transport_worker_lifecycle",
                    "ai_stream_lifecycle", "ai_pending_maintenance",
                    "ai_ignore_hangup", "environment_journal_absent", "environment_journal_pending",
                    "environment_journal_corrupt", "preferences_choice_reselect", "preferences_choice_invalid", "profile_video",
                    "preferences_write_failure", "preferences_cold_missing", "preferences_cold_namespace_timeout",
                    "preferences_cold_volume_timeout", "recognition_query_wire", "recognition_upload_probe",
                    "recognition_audio_saturation",
                    *S3_DISCOVERY_TESTS, *S3_PROFILE_MODEL_TESTS)
NATIVE_PREFERENCES_TESTS = ("preferences_write_failure", "preferences_unrelated_write",
                            "preferences_choice_reselect",
                            *(f"preferences_cold_{case}" for case in
                              ("missing", "valid", "range", "type", "timeout", "priority")))
SPECIAL_TESTS = {
    "xiaotai-szpi-s3": S3_STARTUP_TESTS,
    "xiaotai-cores3-lite": (*S3_STARTUP_TESTS, "rtc_release", "rtc_release_timeout",
                           "rtc_release_twice", "rtc_release_low_memory", "wechat_rx", "wechat_answer_opaque"),
    "xiaotai-waveshare-p4-rev3": ("history_clear", "history_first", "history_maintenance", "stack_business",
                                 "room_maintenance", "transport_worker_lifecycle", "companion_driver_start_failure",
                                 "companion_recovery_no_link", "companion_recovery_unsupported",
                                 "startup_reserve_oom",
                                 *NATIVE_PREFERENCES_TESTS,
                                 *SERVICE_POLICY_TESTS),
    "xiaotai-wt99p461-s1-1v1": ("history_clear", "history_first", "history_maintenance", "presenter",
                              "presenter_binding", "stack_business", "room_maintenance", "preferences_reselect",
                              "transport_worker_lifecycle", "companion_driver_start_failure", "startup_reserve_oom",
                              "companion_recovery_no_link", "companion_recovery_unsupported",
                              *NATIVE_PREFERENCES_TESTS, *SERVICE_POLICY_TESTS,
                              "preferences_cold_peer_empty", "preferences_cold_peer_oversize"),
    "xiaotai-xmini-c3-v3": ("rtc_release", "rtc_release_timeout", "rtc_release_twice",
                         "ai_fixed_rx", "ai_mono_elapsed", "wechat_rx", "wechat_authorization_double",
                         "home_transport_label", "wechat_answer_strict",
                         "wechat_authorization_back", "ledger_session", "ledger_provision",
                         "call_cleanup_quota",
                         "reprovision_keys", "reprovision_partial_cold", "runtime_wifi_audio", "runtime_wifi_cancel_scope",
                         *(f"reprovision_barrier_{case}" for case in ("failed", "saved", "repaired")),
                         "key_audio_settings_ready", "key_audio_settings_fresh", "key_audio_settings_stale",
                         "room_unassigned_key_menu", "contacts_empty_refresh_wechat", "contacts_empty_refresh_requests",
                         *(f"contacts_initial_view_{case}" for case in ("success", "error", "unavailable", "reconnect")),
                         *(f"xmini_prefs_{case}" for case in ("revert", "v1", "absent", "retry", "inflight", "debounce",
                                                            "portal_freeze", "portal_retry", "portal_persistent",
                                                            "portal_slow")),
                         *(f"xmini_prefs_record_{case}" for case in ("valid_v1", "valid_v2_off", "valid_v2_on", "absent",
                                                                   "bad_v1_flag", "bad_v2_flag", "bad_version")),
                         *EXCLUSIVE_PROVISION_TESTS, *SERVICE_POLICY_TESTS,
                         *(f"endpoint_policy_recognition_{case}" for case in ("valid", "invalid", "missing"))),
}


def expected_tests(product, case, boot_only, has_at, storage_recovery=False):
    product_id = product.removeprefix("xiaotai-").replace("-", "_")
    required = ("start", "provision_start")
    if case == "default":
        required += ("start_owner_fault",)
        if product == "xiaotai-xmini-c3-v3":
            required += ("start_owner_fault_timeout", "start_owner_fault_generic", "start_owner_fault_no_display")
        if product != "xiaotai-xmini-c3-v3" or storage_recovery:
            required += ("start_nvs_no_free_pages", "start_nvs_new_version")
        if product != "xiaotai-xmini-c3-v3":
            required += ("start_app_fault",)
    if has_at:
        required += ("console",)
        if product == "xiaotai-xmini-c3-v3" and case == "variant-diagnostics":
            required += ("console_cancel",)
    if not boot_only and case == "variant-no-video" and product in ("xiaotai-szpi-s3", "xiaotai-cores3-lite"):
        required += ("profile_video",)
    if has_at and not boot_only and (
        (case == "default" and product in ("xiaotai-szpi-s3", "xiaotai-cores3-lite"))
        or (case == "variant-diagnostics" and product == "xiaotai-xmini-c3-v3")
    ):
        required += ("recognition_config_contract",)
    if case == "default" and not boot_only:
        required += ("arbitration", *SPECIAL_TESTS.get(product, ()))
        if product != "xiaotai-xmini-c3-v3":   # every product drawn with LVGL
            required += ("screens", "screens_setup", "screens_gallery")
        if product == "xiaotai-xmini-c3-v3":
            required += tuple(f"peer_close_{owner}" for owner in (
                "ai", "call", "wechat", "wechat_orderly", "room", "h5", "h5_network"))
        elif product == "xiaotai-cores3-lite":
            required += ("peer_close_wechat_default",)
        if product in SPECIAL_TESTS:
            required += ("binding_grant_durable", *PLATFORM_BINDING_TESTS)
            if product in ("xiaotai-szpi-s3", "xiaotai-cores3-lite"):
                required += tuple(f"binding_scope_{name}" for name in (
                    "fresh", "storage_failed", "storage_timeout", "report_retry", "reconnect",
                    "login_failed", "login_unbound", "confirmed"))
            if product != "xiaotai-xmini-c3-v3":
                required += ("platform_worker_repair", "endpoint_environment_boot_error", "endpoint_environment_binding_failed_bound",
                             "endpoint_environment_binding_failed_unbound", "endpoint_environment_healthy_bound",
                             "endpoint_environment_healthy_unbound")
    return {f"product_{product_id}_{name}" for name in required}


def check_report(report, expected):
    """CTest can exit zero even when a test was skipped."""
    if not report.is_file():
        return "CTest did not write a JUnit report"
    try:
        cases = list(ET.parse(report).iter("testcase"))
    except ET.ParseError as error:
        return f"invalid CTest report: {error}"
    names = [case.get("name") for case in cases]
    if set(names) != expected or len(names) != len(expected):
        missing = sorted(expected - set(names))
        unexpected = sorted(str(name) for name in set(names) - expected)
        return f"unexpected CTest registration: missing={missing}, extra={unexpected}, listed={len(names)}"
    unpassed = [case.get("name") for case in cases if case.get("status") != "run" or
                any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))]
    return f"CTest did not pass: {', '.join(unpassed)}" if unpassed else None


def step(command, log, env):
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"$ {shlex.join(map(str, command))}\n")
        stream.flush()
        return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env).returncode


def locked_sources(products):
    """Resolve the pinned LVGL source once for this matrix, never from build cache."""
    for product in products:
        declaration = json.loads((ROOT / "products" / product / "product.json").read_text(encoding="utf-8"))
        board = ROOT / "boards" / declaration["board"] / "board.json"
        displays = json.loads(board.read_text(encoding="utf-8"))["parts"].get("display", [])
        if any(display["pixel"] == "rgb565" for display in displays):
            value = subprocess.check_output(
                [sys.executable, "-B", SOURCE.parent / "host_roles.py", "lvgl"], text=True).strip()
            if not value or not Path(value).is_dir():
                raise ValueError("verified LVGL source did not resolve to a directory")
            return [f"-DXIAOTAI_HOST_VERIFIED_lvgl={value}"]
    # Explicitly discard a cache argument left by a different product run.
    return ["-UXIAOTAI_HOST_VERIFIED_lvgl"]


def build_product(product, cases, args, cmake, ctest, root, evidence, jobs, use_ninja, sources):
    cache = root / product
    logs = evidence / product
    logs.mkdir()
    env = {**os.environ, "LC_ALL": "C"}
    passed = 0
    for case, capabilities in cases.items():
        label = f"{product}/{case}"
        log = logs / f"{case}.log"
        report = logs / f"{case}.xml"
        expected = expected_tests(product, case, args.boot_only, capabilities["at"], capabilities.get("storage_recovery", False))
        # Keep a stable generator for explicitly retained caches; CMake checks
        # that such a cache belongs to this source tree and generator.
        generator = ["-G", "Ninja"] if use_ninja and not (cache / "CMakeCache.txt").exists() else []
        commands = {
            "configure": [cmake, "-S", SOURCE, "-B", cache, *generator, *sources,
                          f"-DXIAOTAI_HOST_PRODUCT={product}", f"-DXIAOTAI_HOST_PRODUCT_CASE={case}",
                          f"-DXIAOTAI_HOST_PRODUCT_START_ONLY={'ON' if args.boot_only else 'OFF'}",
                          "-DXIAOTAI_HOST_SANITIZE=ON", "-DXIAOTAI_HOST_ALLOW_MISSING=OFF"],
            "build": [cmake, "--build", cache, "--parallel", str(jobs), "--target", "product_host"],
            "test": [ctest, "--test-dir", cache, "--output-on-failure", "--no-tests=error",
                     "--parallel", str(jobs), "--test-output-size-passed", "65536",
                     "--test-output-size-failed", "65536", "--output-junit", report],
        }
        problem = None
        for phase, command in commands.items():
            code = step(command, log, env)
            if code:
                problem = f"{phase} exited {code}"
                break
        problem = problem or check_report(report, expected)
        if problem:
            print(f"FAIL: {label}: {problem}; {log}", flush=True)
        else:
            print(f"PASS: {label}: {len(expected)} tests; {log}", flush=True)
            passed += 1
    return passed


def positive_int(value):
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return result


@contextmanager
def build_directory(retained):
    if retained is not None:
        root = retained.resolve()
        root.mkdir(parents=True, exist_ok=True)
        yield root
    else:
        with tempfile.TemporaryDirectory(prefix="xiaotai-product-host-") as owned:
            yield Path(owned).resolve()


def evidence_directory(args, build_root):
    parent = args.evidence_root
    if parent is None:
        if args.build_root is not None:
            parent = build_root
        else:
            state = Path(os.environ.get("XDG_STATE_HOME", ""))
            if not state.is_absolute():
                state = Path.home() / ".local" / "state"
            parent = state / "xiaotai" / "product-host"
    parent = parent.resolve()
    if args.build_root is None and parent.is_relative_to(build_root):
        raise ValueError("evidence must be outside the automatic build directory")
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="evidence-", dir=parent))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", action="append", metavar="NAME",
                        help="select one or more product manifest names (default: every product)")
    parser.add_argument("--optional", action="store_true",
                        help="also run named variants and independently removable optional modules")
    parser.add_argument("--boot-only", action="store_true",
                        help="local startup diagnosis; never a full product gate")
    parser.add_argument("--build-root", type=Path,
                        help="keep and reuse a dedicated product build root (do not share between invocations)")
    parser.add_argument("--evidence-root", type=Path,
                        help="keep logs and JUnit in a new run directory under this path")
    parser.add_argument("--parallel-products", type=positive_int, default=2,
                        help="maximum independent product builds in flight (default: 2)")
    parser.add_argument("-j", "--jobs", type=positive_int, default=min(8, os.cpu_count() or 1),
                        help="total build/test jobs, split across active products")
    args = parser.parse_args(argv)
    cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
    if os.name != "posix" or not cmake or not ctest:
        parser.error("generated product tests need Linux/WSL, CMake and CTest")
    try:
        inventory = json.loads(subprocess.check_output(
            [sys.executable, "-B", SOURCE / "generate.py", "--inventory"], text=True))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"FAIL: generated product inventory: {error}", file=sys.stderr)
        return 1
    products = list(dict.fromkeys(args.product or inventory))
    unknown = set(products) - inventory.keys()
    if unknown:
        parser.error(f"unknown products: {', '.join(sorted(unknown))}")
    try:
        sources = locked_sources(products)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"FAIL: locked product host dependencies: {error}", file=sys.stderr)
        return 1

    matrix = {product: (inventory[product] if args.optional else {"default": inventory[product]["default"]})
              for product in products}
    # An explicit retained build root can be reused sequentially, but not by two runners.
    import fcntl
    with build_directory(args.build_root) as root, (root / ".matrix.lock").open("a", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f"FAIL: product build root is in use: {root}", file=sys.stderr)
            return 1
        try:
            evidence = evidence_directory(args, root)
        except (OSError, ValueError) as error:
            print(f"FAIL: product evidence directory: {error}", file=sys.stderr)
            return 1
        print(f"Product builds: {root}\nEvidence: {evidence}", flush=True)
        passed = 0
        slots = min(len(matrix), args.parallel_products, args.jobs)
        jobs = max(1, args.jobs // slots)
        ninja = bool(shutil.which("ninja"))
        with ThreadPoolExecutor(max_workers=slots) as workers:
            futures = {workers.submit(build_product, product, cases, args, cmake, ctest, root,
                                        evidence, jobs, ninja, sources): product for product, cases in matrix.items()}
            for future in as_completed(futures):
                try:
                    passed += future.result()
                except OSError as error:
                    print(f"FAIL: {futures[future]}: {error}", file=sys.stderr, flush=True)
    count = sum(map(len, matrix.values()))
    partial = args.boot_only or len(products) != len(inventory)
    result = "PASS" if passed == count else "FAIL"
    if result == "PASS" and partial:
        result = "PARTIAL"
    scope = ("startup only" if args.boot_only else "full scenarios on default selections")
    if args.optional:
        scope += "; named variants and optional-module removals"
    print(f"{result}: generated product host {passed}/{count} configurations ({scope}); {evidence}", flush=True)
    return 0 if passed == count else 1


if __name__ == "__main__":
    sys.exit(main())

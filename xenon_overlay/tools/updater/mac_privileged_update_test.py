#!/usr/bin/env python3
"""Unprivileged native transaction tests. Never registers or runs a daemon.

Run on macOS with Xcode command line tools:
  python3 xenon_overlay/tools/updater/mac_privileged_update_test.py

The fixture-only factory disables Apple-signature/root-owner requirements at
compile time in a temporary executable. Production identity APIs remain strict
and are tested separately. Passing this suite does NOT verify real elevation.
"""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[3]
UTIL = ROOT / "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.mm"
SERVICE = UTIL.with_name("xenon_update_mac_privileged_service.mm")
GENERATION = UTIL.with_name("xenon_update_mac_privileged_generation.mm")

DRIVER = r'''
#import <Foundation/Foundation.h>
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"
#include <stdio.h>
#include <unistd.h>

@interface XenonPrivilegedUpdateService : NSObject
- (instancetype)initWithTarget:(NSString*)target generation:(NSString*)generation;
- (void)prepareServiceForGeneration:(NSString*)generation
    reply:(void (^)(BOOL, NSString*))reply;
- (void)setJobForTesting:(XenonPrivilegedUpdateJob*)job;
- (void)setRefreshCallerPIDForTesting:(pid_t)pid;
@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 2 || geteuid() == 0) return 2;
    NSString* target = [NSString stringWithUTF8String:argv[1]];
    NSString* error = nil;
    XenonPrivilegedUpdateJob* job = [[XenonPrivilegedUpdateJob alloc]
        initForTestingWithTarget:target error:&error];
    if (!job) return 3;
    XenonPrivilegedUpdateService* service = [[XenonPrivilegedUpdateService alloc]
        initWithTarget:target generation:@"fixture-generation-one"];
    NSString* saved_token = nil;
    char* line = nullptr;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stdin)) > 0) {
      @autoreleasepool {
        NSData* data = [NSData dataWithBytes:line length:static_cast<NSUInteger>(length)];
        NSDictionary* request = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        NSString* operation = request[@"op"];
        NSString* token = request[@"token"] ?: saved_token;
        uid_t uid = request[@"uid"] ? [request[@"uid"] unsignedIntValue] : geteuid();
        NSString* error = nil;
        id result = nil;
        if ([operation isEqual:@"prepare"] || [operation isEqual:@"compete"]) {
          XenonPrivilegedUpdateJob* candidate = [operation isEqual:@"compete"]
              ? [[XenonPrivilegedUpdateJob alloc] initForTestingWithTarget:target error:&error] : job;
          result = [candidate preparePath:request[@"source"]
              baseVersion:request[@"base"] ?: @"1.0.0.1"
              targetVersion:request[@"version"] ?: @"1.0.0.2"
              callerPID:[request[@"pid"] intValue] callerUID:uid error:&error];
          if ([operation isEqual:@"prepare"] && result) saved_token = result;
        } else if ([operation isEqual:@"state"]) {
          result = [job stateForToken:token callerUID:uid error:&error];
        } else if ([operation isEqual:@"commit"]) {
          result = @([job commitToken:token callerUID:uid error:&error]);
        } else if ([operation isEqual:@"rollback"]) {
          result = @([job rollbackToken:token callerUID:uid error:&error]);
        } else if ([operation isEqual:@"finish"]) {
          result = @([job finishToken:token callerUID:uid
              preserveBackup:[request[@"preserve"] boolValue] error:&error]);
        } else if ([operation isEqual:@"identity"]) {
          result = xenon::updater::ReadTrustedCodeIdentity(request[@"path"], &error);
        } else if ([operation isEqual:@"requirement"]) {
          result = xenon::updater::PrivilegedPeerRequirement(request[@"team"], request[@"ids"], &error);
        } else if ([operation isEqual:@"production"]) {
          result = @([[XenonPrivilegedUpdateJob alloc] initWithTarget:target error:&error] != nil);
        } else if ([operation isEqual:@"expire"]) {
          [job expireLeaseForTesting];
          result = @([job reclaimIfExpired]);
        } else if ([operation isEqual:@"reclaim"]) {
          result = @([job reclaimUncommittedForCallerUID:uid]);
        } else if ([operation isEqual:@"service_job"]) {
          [service setJobForTesting:job];
          result = @YES;
        } else if ([operation isEqual:@"service_caller"]) {
          [service setRefreshCallerPIDForTesting:[request[@"pid"] intValue]];
          result = @YES;
        } else if ([operation isEqual:@"service_generation"]) {
          dispatch_semaphore_t done = dispatch_semaphore_create(0);
          __block BOOL refresh = NO;
          __block NSString* service_error = nil;
          [service prepareServiceForGeneration:request[@"generation"] reply:^(BOOL value, NSString* message) {
            refresh = value;
            service_error = message;
            dispatch_semaphore_signal(done);
          }];
          if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) return 4;
          result = @(refresh);
          error = service_error;
        }
        NSData* response = [NSJSONSerialization dataWithJSONObject:
            @{@"result": result ?: NSNull.null, @"error": error ?: NSNull.null}
            options:0 error:nil];
        fwrite(response.bytes, 1, response.length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
      }
    }
    free(line);
  }
}
'''


@unittest.skipUnless(os.uname().sysname == "Darwin" and os.geteuid() != 0,
                     "requires macOS and an unprivileged user")
class PrivilegedUpdateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="xenon-privileged-tests-")
        driver = Path(cls.build.name) / "driver.mm"
        driver.write_text(DRIVER)
        cls.executable = Path(cls.build.name) / "driver"
        subprocess.run([
            "xcrun", "clang++", "-std=c++17", "-fobjc-arc",
            "-mmacosx-version-min=13.0", "-DXENON_PRIVILEGED_UPDATE_TESTING",
            "-I", str(ROOT), str(driver), str(UTIL), str(SERVICE), str(GENERATION),
            "-framework", "Foundation",
            "-framework", "Security", "-o", str(cls.executable),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xenon-privileged-fixture-")
        self.root = Path(self.temporary.name)
        self.target = self.root / "Installed.app"
        self.source = self.root / "Incoming.app"
        self.make_app(self.target, "1.0.0.1")
        self.make_app(self.source, "1.0.0.2")
        self.browser = subprocess.Popen(["/bin/sleep", "120"])
        self.driver = subprocess.Popen([str(self.executable), str(self.target)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)

    def tearDown(self):
        self.exit_browser()
        if self.driver.poll() is None:
            self.driver.stdin.close()
            self.driver.wait(timeout=10)
        self.driver.stdout.close()
        self.driver.stderr.close()
        self.temporary.cleanup()

    def make_app(self, app, version):
        executable = app / "Contents/MacOS/browser"
        executable.parent.mkdir(parents=True)
        executable.write_text("fixture " + version)
        executable.chmod(0o755)
        info = {"CFBundleIdentifier": "com.example.xenon", "CFBundleExecutable": "browser",
                "CFBundleShortVersionString": version, "CFBundlePackageType": "APPL"}
        with (app / "Contents/Info.plist").open("wb") as stream:
            plistlib.dump(info, stream)
        for directory in [app, app / "Contents", executable.parent]:
            directory.chmod(0o755)

    def version(self, app=None):
        with ((app or self.target) / "Contents/Info.plist").open("rb") as stream:
            return plistlib.load(stream)["CFBundleShortVersionString"]

    def request(self, operation, **values):
        self.driver.stdin.write(json.dumps({"op": operation, **values}) + "\n")
        self.driver.stdin.flush()
        line = self.driver.stdout.readline()
        if not line:
            self.fail("native fixture exited: " + self.driver.stderr.read())
        return json.loads(line)

    def prepare(self, **values):
        return self.request("prepare", source=str(self.source), pid=self.browser.pid, **values)

    def exit_browser(self):
        if self.browser.poll() is None:
            self.browser.terminate()
            self.browser.wait(timeout=10)

    def ready(self):
        self.exit_browser()
        for _ in range(50):
            if self.request("state")["result"] == "ready":
                return
            time.sleep(0.01)
        self.fail("kqueue did not report the authenticated caller's exit")

    def test_waits_for_exit_commits_and_rolls_back_atomically(self):
        original = self.target.stat().st_ino
        source_inode = self.source.stat().st_ino
        self.assertIsNotNone(self.prepare()["result"])
        self.assertEqual(self.request("state")["result"], "prepared")
        self.assertFalse(self.request("commit")["result"])
        self.assertEqual(self.version(), "1.0.0.1")
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.assertEqual(self.version(), "1.0.0.2")
        self.assertNotEqual(self.target.stat().st_ino, source_inode)
        self.assertEqual(self.source.stat().st_ino, source_inode)
        self.assertTrue(self.request("rollback")["result"])
        self.assertEqual(self.target.stat().st_ino, original)
        self.assertTrue(self.request("finish")["result"])
        self.assertFalse(list(self.root.rglob(".xenon-privileged-*")))

    def test_success_preserves_backup_when_requested(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.assertTrue(self.request("finish", preserve=True)["result"])
        backups = list(self.root.glob("PrivateStaging/.xenon-privileged-*/Installed.app"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(self.version(backups[0]), "1.0.0.1")
        self.assertEqual(self.version(), "1.0.0.2")
        self.assertIsNone(self.request("state")["result"])

    def test_token_uid_and_exclusive_lock_are_enforced(self):
        token = self.prepare()["result"]
        self.assertIsNotNone(token)
        self.assertIsNone(self.request("state", uid=os.geteuid() + 1)["result"])
        self.assertIsNone(self.request("state", token="invalid")["result"])
        self.assertFalse(self.request("finish", uid=os.geteuid() + 1)["result"])
        self.assertIsNone(self.request("compete", source=str(self.source),
                                       pid=self.browser.pid)["result"])
        self.assertEqual(self.request("state")["result"], "prepared")
        self.assertTrue(self.request("finish")["result"])

    def test_changed_target_inode_blocks_commit(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.target.rename(self.root / "Previous.app")
        self.make_app(self.target, "1.0.0.3")
        current = self.target.stat().st_ino
        self.assertFalse(self.request("commit")["result"])
        self.assertEqual(self.target.stat().st_ino, current)
        self.assertEqual(self.version(), "1.0.0.3")
        self.assertTrue(self.request("finish")["result"])

    def test_later_installation_is_not_overwritten_by_rollback(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.target.rename(self.root / "Committed.app")
        self.make_app(self.target, "1.0.0.3")
        self.assertFalse(self.request("rollback")["result"])
        self.assertEqual(self.version(), "1.0.0.3")
        self.assertFalse(self.request("finish", preserve=True)["result"])
        self.assertEqual(self.version(next(self.root.glob("PrivateStaging/.xenon-privileged-*/Installed.app"))),
                         "1.0.0.1")

    def test_private_copy_survives_changes_to_user_source(self):
        prepared = self.prepare()
        self.assertIsNotNone(prepared["result"], prepared)
        shutil.rmtree(self.source)
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.assertEqual(self.version(), "1.0.0.2")
        self.assertTrue(self.request("finish")["result"])

    def test_internal_framework_symlinks_are_preserved(self):
        framework = self.source / "Contents/Frameworks/Test.framework"
        (framework / "Versions/1.0.0.2").mkdir(parents=True)
        (framework / "Versions/1.0.0.2/Test").write_text("framework")
        (framework / "Versions/Current").symlink_to("1.0.0.2")
        (framework / "Test").symlink_to("Versions/Current/Test")
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.assertEqual((self.target / "Contents/Frameworks/Test.framework/Test").read_text(),
                         "framework")
        self.assertTrue(self.request("finish")["result"])

    def test_rejects_escaping_symlink_without_touching_victim(self):
        victim = self.root / "victim"
        victim.write_text("keep")
        (self.source / "Contents/escape").symlink_to("../../victim")
        self.assertIsNone(self.prepare()["result"])
        self.assertEqual(victim.read_text(), "keep")
        self.assertEqual(self.version(), "1.0.0.1")

    def test_rejects_hardlinks_setuid_and_special_files(self):
        extra = self.source / "Contents/extra"
        os.link(self.source / "Contents/MacOS/browser", extra)
        self.assertIsNone(self.prepare()["result"])
        extra.unlink()
        extra.write_text("unsafe")
        # macOS sandbox may strip setuid/setgid on chmod. Sticky exercises the
        # same forbidden special-mode mask without requesting setuid execution.
        extra.chmod(0o1755)
        self.assertIsNone(self.prepare()["result"])
        extra.unlink()
        os.mkfifo(extra)
        self.assertIsNone(self.prepare()["result"])
        self.assertEqual(self.version(), "1.0.0.1")

    def test_rejects_bad_versions_and_symlink_source(self):
        self.assertIsNone(self.prepare(version="1.0.0.1")["result"])
        self.assertIsNone(self.prepare(base="1.0.0.0")["result"])
        self.assertIsNone(self.prepare(version="1.0.0.3")["result"])
        alias = self.root / "Alias.app"
        alias.symlink_to(self.source)
        self.assertIsNone(self.request("prepare", source=str(alias),
                                       pid=self.browser.pid)["result"])

    def test_signature_and_requirement_fail_closed(self):
        self.assertFalse(self.request("production")["result"])
        binary = self.source / "Contents/MacOS/browser"
        shutil.copyfile("/usr/bin/true", binary)
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(binary)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertIsNone(self.request("identity", path=str(binary))["result"])
        self.assertIsNone(self.request("requirement", team='TEAM" or true',
                                       ids=["com.example.xenon"])["result"])
        result = self.request("requirement", team="ABCDEFGHIJ", ids=["com.example.xenon"])
        self.assertIn("anchor apple generic", result["result"])

    def test_expired_unclaimed_job_releases_lock_and_only_discards_copy(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.assertTrue(self.request("expire")["result"])
        self.assertEqual(self.version(), "1.0.0.1")
        self.assertFalse(list(self.root.rglob(".xenon-privileged-*")))
        self.assertIsNotNone(self.prepare()["result"])
        self.assertTrue(self.request("finish")["result"])

    def test_lost_unclaimed_token_can_be_reclaimed_after_caller_exit(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.exit_browser()
        self.assertTrue(self.request("reclaim")["result"])
        self.assertEqual(self.version(), "1.0.0.1")
        self.assertIsNone(self.request("state")["result"])

    def test_claimed_live_transaction_is_not_reclaimed_by_another_prepare(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertFalse(self.request("reclaim")["result"])
        self.assertEqual(self.request("state")["result"], "ready")
        self.assertTrue(self.request("finish")["result"])

    def test_expired_committed_job_keeps_installed_app_and_backup(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        self.assertTrue(self.request("expire")["result"])
        self.assertEqual(self.version(), "1.0.0.2")
        backup = next(self.root.glob("PrivateStaging/.xenon-privileged-*/Installed.app"))
        self.assertEqual(self.version(backup), "1.0.0.1")
        self.assertIsNone(self.request("state")["result"])

    def test_finish_keeps_only_recovery_copy_if_target_was_removed(self):
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        shutil.rmtree(self.target)
        self.assertFalse(self.request("finish", preserve=False)["result"])
        backup = next(self.root.glob("PrivateStaging/.xenon-privileged-*/Installed.app"))
        self.assertEqual(self.version(backup), "1.0.0.1")

    def test_other_main_process_blocks_commit_until_it_exits(self):
        main = self.target / "Contents/MacOS/browser"
        shutil.copyfile(self.executable, main)
        other = subprocess.Popen([str(main), str(self.target)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertIsNotNone(self.prepare()["result"])
            self.ready()
            result = self.request("commit")
            self.assertFalse(result["result"])
            self.assertIn("Another instance", result["error"])
            self.assertEqual(self.version(), "1.0.0.1")
            other.stdin.close()
            other.wait(timeout=10)
            self.assertTrue(self.request("commit")["result"])
            self.assertTrue(self.request("finish")["result"])
        finally:
            if other.poll() is None:
                other.terminate()
                other.wait(timeout=10)
            other.stdout.close()
            other.stderr.close()

    def test_writable_old_bundle_is_retained_after_success(self):
        resources = self.target / "Contents/Resources"
        resources.mkdir()
        resources.chmod(0o777)
        (resources / "keep").write_text("old recovery content")
        self.assertIsNotNone(self.prepare()["result"])
        self.ready()
        self.assertTrue(self.request("commit")["result"])
        result = self.request("finish", preserve=False)
        self.assertFalse(result["result"])
        self.assertIn("retaining", result["error"])
        backup = next(self.root.glob("PrivateStaging/.xenon-privileged-*/Installed.app"))
        self.assertEqual((backup / "Contents/Resources/keep").read_text(),
                         "old recovery content")
        self.assertEqual(self.version(), "1.0.0.2")

    def test_root_staging_fixture_rejects_symlink_anchor(self):
        victim = self.root / "Victim"
        victim.mkdir()
        (victim / "keep").write_text("keep")
        (self.root / "PrivateStaging").symlink_to(victim)
        self.assertIsNone(self.prepare()["result"])
        self.assertEqual((victim / "keep").read_text(), "keep")
        self.assertEqual(self.version(), "1.0.0.1")

    def test_generation_refresh_is_drained_and_retryable_by_its_caller(self):
        result = self.request("service_generation", generation="fixture-generation-one")
        self.assertFalse(result["result"])
        self.assertIsNone(result["error"])
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertTrue(result["result"])
        self.assertIsNone(result["error"])
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertTrue(result["result"])
        self.assertIsNone(result["error"])
        self.request("service_caller", pid=self.browser.pid)
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertFalse(result["result"])
        self.assertIn("Another running browser", result["error"])

    def test_generation_refresh_can_recover_after_its_caller_exits(self):
        self.request("service_caller", pid=self.browser.pid)
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertTrue(result["result"])
        self.exit_browser()
        self.request("service_caller", pid=0)
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertTrue(result["result"])
        self.assertIsNone(result["error"])

    def test_generation_refresh_refuses_active_transaction_and_empty_generation(self):
        result = self.request("service_generation", generation="")
        self.assertFalse(result["result"])
        self.assertIsNotNone(result["error"])
        self.assertIsNotNone(self.prepare()["result"])
        self.request("service_job")
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertFalse(result["result"])
        self.assertIn("transaction is active", result["error"])
        self.assertTrue(self.request("finish")["result"])
        result = self.request("service_generation", generation="fixture-generation-two")
        self.assertTrue(result["result"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

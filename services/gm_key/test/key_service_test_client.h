// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_TEST_KEY_SERVICE_TEST_CLIENT_H_
#define SERVICES_GM_KEY_TEST_KEY_SERVICE_TEST_CLIENT_H_

#include "base/files/file_path.h"
#include "base/process/process.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"
#include "services/gm_key/test/gm_key_test_control.test-mojom.h"

namespace gm_key::test {

// The caller initializes Mojo and provides an IO task runner/ScopedIPCSupport.
// This is test-only: the production utility never exposes KeyTestControl.
class ServiceProcess {
 public:
  ServiceProcess();
  ~ServiceProcess();
  ServiceProcess(const ServiceProcess&) = delete;
  ServiceProcess& operator=(const ServiceProcess&) = delete;
  bool Launch(const base::FilePath& executable);
  mojo::Remote<gm_key::mojom::KeyService>& service() { return service_; }
  mojo::Remote<gm_key::test::mojom::KeyTestControl>& control() {
    return control_;
  }
  base::Process& process() { return process_; }

 private:
  base::Process process_;
  mojo::Remote<gm_key::mojom::KeyService> service_;
  mojo::Remote<gm_key::test::mojom::KeyTestControl> control_;
};

}  // namespace gm_key::test
#endif  // SERVICES_GM_KEY_TEST_KEY_SERVICE_TEST_CLIENT_H_

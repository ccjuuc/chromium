// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const path = require('node:path');
const {buildBootstrap} = require('../../tools/bundle_ipc_bootstrap.cjs');

function readBootstrap(filename) {
  return buildBootstrap(filename).source;
}

function readBootstrapPart(filename) {
  return readBootstrap(path.join(__dirname, filename));
}

module.exports = {readBootstrap, readBootstrapPart};

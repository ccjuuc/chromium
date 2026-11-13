// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createPowerSaveBlockerApi({callHost}) {
  const invalid = message => Object.assign(new TypeError(message),
    {code: 'ERR_INVALID_ARG_TYPE'});
  function checkId(id) {
    if (!Number.isInteger(id) || id < -2147483648 || id > 2147483647)
      throw invalid('powerSaveBlocker id must be a 32-bit integer');
  }
  return {
    start(type) {
      if (type !== 'prevent-app-suspension' && type !== 'prevent-display-sleep')
        throw invalid('Invalid powerSaveBlocker type');
      return callHost({method: 'start', type});
    },
    stop(id) {
      checkId(id);
      return callHost({method: 'stop', id});
    },
    isStarted(id) {
      checkId(id);
      return callHost({method: 'isStarted', id});
    },
  };
}

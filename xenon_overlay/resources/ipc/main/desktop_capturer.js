// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createDesktopCapturer({callHost, nativeImage, isReady}) {
  const invalid = message => Object.assign(new TypeError(message), {code:'ERR_INVALID_ARG_TYPE'});
  return {
    async getSources(options) {
      if (!isReady()) throw Object.assign(new Error('desktopCapturer requires app readiness'), {code:'ERR_NOT_READY'});
      if (!options || typeof options !== 'object' || Array.isArray(options) || !Array.isArray(options.types))
        throw invalid('getSources requires options.types');
      const types = [...new Set(options.types)];
      if (types.some(type => type !== 'screen' && type !== 'window'))
        throw invalid('source type must be screen or window');
      const size = options.thumbnailSize === undefined ? {width:150,height:150} : options.thumbnailSize;
      if (!size || !Number.isInteger(size.width) || !Number.isInteger(size.height) ||
          size.width < 0 || size.height < 0 || size.width > 4096 || size.height > 4096)
        throw invalid('thumbnailSize must contain dimensions between 0 and 4096');
      if (options.fetchWindowIcons !== undefined && typeof options.fetchWindowIcons !== 'boolean')
        throw invalid('fetchWindowIcons must be a boolean');
      const results = await callHost({types, width:size.width, height:size.height,
        fetchWindowIcons:options.fetchWindowIcons === true});
      const decode = png => png ? nativeImage.createFromBuffer(Buffer.from(png, 'base64')) : nativeImage.createEmpty();
      return results.map(source => ({id:source.id,name:source.name,display_id:source.display_id,
        thumbnail:decode(source.thumbnail),appIcon:source.appIcon === null ? null : decode(source.appIcon)}));
    },
  };
}

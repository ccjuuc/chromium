  const netModule = createXenonNetModule({
    platform: __xenonPlatform, idPrefix: 'm-',
    send: (channel, payload) => __xenonNetSend(channel, payload),
    setDispatch: callback => { dispatchXenonNet = callback; },
  });

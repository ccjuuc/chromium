  const netModule = createXenonNetModule({
    platform: process.platform, idPrefix: 'r-',
    send: (channel, payload) => transport.send(channel, payload),
    setDispatch: callback => { dispatchXenonNet = callback; },
  });

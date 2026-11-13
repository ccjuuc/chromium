  const fsModule = createNativeFs(
      (request, data) => __xenonFsCall(request, data),
      (request, data) => __xenonFsCallAsync(request, data));
  const fsPromises = fsModule.promises;
  const fsConstants = fsModule.constants;
  let originalFsModule;
  function getOriginalFsModule() {
    return originalFsModule ||= createNativeFs(
        (request, data) => __xenonFsCall({...request, noAsar: true}, data),
        (request, data) => __xenonFsCallAsync({...request, noAsar: true}, data));
  }

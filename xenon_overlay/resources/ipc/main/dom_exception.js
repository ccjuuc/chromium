  function createMainDOMExceptionClass() {
    const states = new WeakMap();
    const names = [
      ['IndexSizeError', 'INDEX_SIZE_ERR'], ['DOMStringSizeError', 'DOMSTRING_SIZE_ERR'],
      ['HierarchyRequestError', 'HIERARCHY_REQUEST_ERR'], ['WrongDocumentError', 'WRONG_DOCUMENT_ERR'],
      ['InvalidCharacterError', 'INVALID_CHARACTER_ERR'], ['NoDataAllowedError', 'NO_DATA_ALLOWED_ERR'],
      ['NoModificationAllowedError', 'NO_MODIFICATION_ALLOWED_ERR'], ['NotFoundError', 'NOT_FOUND_ERR'],
      ['NotSupportedError', 'NOT_SUPPORTED_ERR'], ['InUseAttributeError', 'INUSE_ATTRIBUTE_ERR'],
      ['InvalidStateError', 'INVALID_STATE_ERR'], ['SyntaxError', 'SYNTAX_ERR'],
      ['InvalidModificationError', 'INVALID_MODIFICATION_ERR'], ['NamespaceError', 'NAMESPACE_ERR'],
      ['InvalidAccessError', 'INVALID_ACCESS_ERR'], ['ValidationError', 'VALIDATION_ERR'],
      ['TypeMismatchError', 'TYPE_MISMATCH_ERR'], ['SecurityError', 'SECURITY_ERR'],
      ['NetworkError', 'NETWORK_ERR'], ['AbortError', 'ABORT_ERR'],
      ['URLMismatchError', 'URL_MISMATCH_ERR'], ['QuotaExceededError', 'QUOTA_EXCEEDED_ERR'],
      ['TimeoutError', 'TIMEOUT_ERR'], ['InvalidNodeTypeError', 'INVALID_NODE_TYPE_ERR'],
      ['DataCloneError', 'DATA_CLONE_ERR'],
    ];
    const codes = new Map(names.map(([name], index) => [name, index + 1]));
    // The constants remain for compatibility, but these obsolete exception
    // names are no longer part of the WebIDL legacy code mapping.
    for (const name of ['DOMStringSizeError', 'NoDataAllowedError', 'ValidationError']) codes.delete(name);
    function state(value) {
      const result = states.get(value);
      if (!result) throw Object.assign(new TypeError('Value of this must be DOMException'), {code: 'ERR_INVALID_THIS'});
      return result;
    }
    class DOMException extends Error {
      constructor(message = '', name = 'Error') {
        super();
        states.set(this, {message: `${message}`, name: `${name}`});
      }
      get name() { return state(this).name; }
      get message() { return state(this).message; }
      get code() { return codes.get(state(this).name) || 0; }
    }
    for (const name of ['name', 'message', 'code']) {
      Object.defineProperty(DOMException.prototype, name,
        {...Object.getOwnPropertyDescriptor(DOMException.prototype, name), enumerable: true});
    }
    Object.defineProperty(DOMException.prototype, Symbol.toStringTag, {value: 'DOMException', configurable: true});
    for (const [index, [, name]] of names.entries()) {
      const descriptor = {value: index + 1, enumerable: true};
      Object.defineProperty(DOMException, name, descriptor);
      Object.defineProperty(DOMException.prototype, name, descriptor);
    }
    return DOMException;
  }

// VT control character stripping adapted from Node.js v24.19.0,
// lib/internal/util/inspect.js.
//
// Copyright Node.js contributors. All rights reserved.
// Copyright Joyent, Inc. and other Node contributors. All rights reserved.
// ANSI expression: Copyright Sindre Sorhus <sindresorhus@gmail.com>.
// https://github.com/chalk/ansi-regex/blob/f338e1814144efb950276aac84135ff86b72dc8e/index.js
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

const stripVTControlCharacters = (() => {
  const indexOf = Function.prototype.call.bind(String.prototype.indexOf);
  const replace = Function.prototype.call.bind(RegExp.prototype[Symbol.replace]);
  // Keep Node's CSI/OSC grammar, including BEL and both ST terminators.
  const ansi = new RegExp(
    '[\\u001B\\u009B][[\\]()#;?]*' +
    '(?:(?:(?:(?:;[-a-zA-Z\\d\\/\\#&.:=?%@~_]+)*' +
    '|[a-zA-Z\\d]+(?:;[-a-zA-Z\\d\\/\\#&.:=?%@~_]*)*)?' +
    '(?:\\u0007|\\u001B\\u005C|\\u009C))' +
    '|(?:(?:\\d{1,4}(?:;\\d{0,4})*)?' +
    '[\\dA-PR-TZcf-nq-uy=><~]))', 'g');

  return function stripVTControlCharacters(str) {
    if (typeof str !== 'string') {
      const error = new TypeError('The "str" argument must be of type string');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
    if (indexOf(str, '\u001B') === -1 && indexOf(str, '\u009B') === -1)
      return str;
    return replace(ansi, str, '');
  };
})();

'use strict';

var assert = require('assert');

// testing handle scope api calls
var testHandleScope =
    require(`./build/Release/test_handle_scope`);

testHandleScope.NewScope();

assert.ok(testHandleScope.NewScopeEscape() instanceof Object);

testHandleScope.NewScopeEscapeTwice();

assert.throws(
  () => {
    testHandleScope.NewScopeWithException(() => { throw new RangeError(); });
  },
  RangeError);

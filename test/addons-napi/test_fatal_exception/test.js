'use strict';

var assert = require('assert');
var test_fatal = require(`./build/Release/test_fatal_exception`);

process.on('uncaughtException', common.mustCall(function(err) {
  assert.strictEqual(err.message, 'fatal error');
}));

var err = new Error('fatal error');
test_fatal.Test(err);

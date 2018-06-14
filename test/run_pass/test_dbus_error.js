var assert = require('assert');
var dbus = require('dbus');
var bus = dbus.getBus();

bus.getInterface('not.exits', '/path', 'myname', function(err) {
  // assert.strictEqual(err.message, 'service name is invalid, please use x.y.z');
  bus.destroy();

  bus.reconnect();
  console.log('reconnect done');
  bus.getInterface('not.exist2', '/path', 'myname', function(err) {
    console.log('error');
    // assert.strictEqual(err.message, 'no introspectable found');
    bus.destroy();
  });
});

// Promise.resolve()
//   .then(function() {
//     return getInterface('not exist', '/path', 'myname');
//   })
//   .then(function(err) {
//     assert.strictEqual(err.message, 'service name is invalid, please use x.y.z');
//   })
//   // .then(function() {
//   //   return bus.getInterface('not.exist', '/path', 'myname');
//   // })
//   // .then(function(err) {
//   //   assert.strictEqual(err.message, 'no introspectable found');
//   // })
//   .then(function() {
//     console.log(123);
//     bus.dbus.releaseBus();
//   });

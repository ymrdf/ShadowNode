'use strict';

/**
 * @class Snapshot
 */
function Snapshot() {
  // native.takeSnapshot();
  throw new Error('not implemented');
}

/**
 * @method getHeader
 */
Snapshot.prototype.getHeader = function() {
  // TODO
  throw new Error('not implemented');
};

/**
 * @method compare
 */
Snapshot.prototype.compare = function(snapshot) {
  // TODO
  throw new Error('not implemented');
};

/**
 * @method export
 */
Snapshot.prototype.export = function(cb) {
  // TODO
  throw new Error('not implemented');
};

/**
 * @method serialize
 */
Snapshot.prototype.serialize = function() {
  // TODO
  throw new Error('not implemented');
};

/**
 * @class Profile
 */
function Profile() {
  // TODO
}

/**
 * @method getHeader
 */
Profile.prototype.getHeader = function() {
  throw new Error('not implemented');
};

/**
 * @method delete
 */
Profile.prototype.delete = function() {
  throw new Error('not implemented');
};

/**
 * @method export
 */
Profile.prototype.export = function(cb) {
  throw new Error('not implemented');
};

/**
 * @method getHeader
 */
function takeSnapshot() {
  return new Snapshot();
}

/**
 * startProfiling(type)
 * startProfiling(type, path)
 * startProfiling(type, duration)
 * startProfiling(type, path, duration)
 * type:
 * NONE_CPU_PROFILER = 0
 * JS_CPU_PROFILER = 1
 * BUILTIN_CPU_PROFILER = 2
 * ALLOC_CPU_PROFILER = 3
 * GC_CPU_PROFILER = 4
 */
function startProfiling() {
  var type = arguments[0];
  var path = `${process.cwd()}/Profile-${Date.now()}`;
  var duration = -1;

  if (arguments.length === 2) {
    if (typeof (arguments[1]) === 'number') {
      duration = arguments[1];
    } else if (typeof (arguments[1]) === 'string') {
      path = arguments[1];
    }
  } else if (arguments.length === 3) {
    path = arguments[1];
    duration = arguments[2];
  }

  native.startProfiling(type, path, duration);
}

function stopProfiling() {
  native.stopProfiling();
  return new Profile();
}

exports.takeSnapshot = takeSnapshot;
exports.startProfiling = startProfiling;
exports.stopProfiling = stopProfiling;

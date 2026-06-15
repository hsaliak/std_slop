
(function() {
  function requireObject(name, value) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
      throw new TypeError(name + ' args must be an object');
    }
  }

  function requireString(toolName, args, field) {
    if (typeof args[field] !== 'string') {
      throw new TypeError(toolName + ' requires string field ' + field);
    }
  }

  function call(name, args) {
    const safeArgs = args === undefined ? {} : args;
    requireObject(name, safeArgs);
    return globalThis.call_tool(name, safeArgs);
  }

  const helpers = {
    dispatch(name, args = {}) {
      if (typeof name !== 'string' || name.length === 0) {
        throw new TypeError('tools.dispatch requires a non-empty tool name');
      }
      return call(name, args);
    },

    help(args = {}) {
      requireObject('help', args);
      return {
        tools: [
          'dispatch', 'help', 'read_file', 'list_directory', 'grep',
          'llm_query'
        ],
        note: 'Use tools.dispatch(name, args) for host tools without a JS helper.'
      };
    },

    read_file(args = {}) {
      requireObject('read_file', args);
      requireString('read_file', args, 'path');
      return call('read_file', args);
    },

    list_directory(args = {}) {
      requireObject('list_directory', args);
      requireString('list_directory', args, 'path');
      return call('list_directory', args);
    },

    grep(args = {}) {
      requireObject('grep', args);
      requireString('grep', args, 'path');
      requireString('grep', args, 'pattern');
      return call('grep', args);
    },

    llm_query(args = {}) {
      requireObject('llm_query', args);
      requireString('llm_query', args, 'query');
      return call('llm_query', args);
    }
  };

  globalThis.tools = new Proxy(helpers, {
    get(target, property) {
      if (typeof property !== 'string') return undefined;
      if (Object.prototype.hasOwnProperty.call(target, property)) return target[property];
      return function(args = {}) { return globalThis.call_tool(property, args); };
    }
  });
})();

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

  function requireBoolean(toolName, args, field) {
    if (typeof args[field] !== 'boolean') {
      throw new TypeError(toolName + ' requires boolean field ' + field);
    }
  }

  const payloadKeyOnlyTools = ['edit_tool', 'write_file'];
  const rawCallTool = globalThis.call_tool;

  function isPayloadKeyOnlyTool(name) {
    return payloadKeyOnlyTools.indexOf(name) !== -1;
  }

  function call(name, args) {
    const safeArgs = args === undefined ? {} : args;
    requireObject(name, safeArgs);
    return rawCallTool(name, safeArgs);
  }

  function requireInputPayload(toolName, key) {
    if (typeof key !== 'string' || key.length === 0) {
      throw new TypeError(
          'tools.' + toolName + ' inside run_js only accepts a non-empty input key string; ' +
          'place the full ' + toolName + ' request at run_js.input[key]');
    }
    if (globalThis.input === null || typeof globalThis.input !== 'object' || Array.isArray(globalThis.input)) {
      throw new TypeError('tools.' + toolName + ' requires run_js.input to be an object');
    }
    if (!Object.prototype.hasOwnProperty.call(globalThis.input, key)) {
      throw new TypeError('tools.' + toolName + '("' + key + '") could not find input.' + key);
    }
    const payload = globalThis.input[key];
    requireObject(toolName, payload);
    return payload;
  }

  function rejectPayloadToolBypass(name) {
    if (isPayloadKeyOnlyTool(name)) {
      throw new TypeError(
          'tools.' + name + ' inside run_js only accepts an input key string; ' +
          'use tools.' + name + '("input_key") and place the full request at run_js.input[input_key]');
    }
  }

  globalThis.call_tool = function(name, args) {
    if (typeof name !== 'string' || name.length === 0) {
      throw new TypeError('call_tool name must be a non-empty string');
    }
    rejectPayloadToolBypass(name);
    return call(name, args);
  };

  const helperNames = [
    'dispatch', 'help', 'persist_function', 'read_file', 'list_directory',
    'grep', 'write_file', 'edit_tool', 'execute_bash', 'llm_query'
  ];

  function staticHelp() {
    return {
      helpers: helperNames,
      persisted_globals: [],
      tools: helperNames,
      note: 'Use tools.dispatch(name, args) for host tools without a JS helper.'
    };
  }

  const helpers = {
    dispatch(name, args = {}) {
      if (typeof name !== 'string' || name.length === 0) {
        throw new TypeError('tools.dispatch requires a non-empty tool name');
      }
      rejectPayloadToolBypass(name);
      return call(name, args);
    },

    help(args = {}) {
      requireObject('help', args);
      let rows;
      try {
        rows = call('query_db', {
          sql: 'SELECT name, description, json_schema, is_enabled, is_top_level, is_run_js_callable FROM tools WHERE is_enabled = 1 ORDER BY name'
        });
      } catch (err) {
        return staticHelp();
      }
      if (!Array.isArray(rows)) return staticHelp();

      let persistedGlobals = [];
      try {
        const persistedRows = call('query_db', {
          sql: 'SELECT name, description, json_schema FROM js_functions ORDER BY name'
        });
        if (Array.isArray(persistedRows)) {
          persistedGlobals = persistedRows.map(function(row) {
            return {
              name: row.name,
              description: row.description || '',
              schema: row.json_schema || ''
            };
          });
        }
      } catch (err) {
        persistedGlobals = [];
      }

      const normalized = rows.map(function(row) {
        return {
          name: row.name,
          description: row.description,
          schema: row.json_schema,
          top_level: row.is_top_level === 1 || row.is_top_level === true,
          run_js_callable: row.is_run_js_callable === 1 || row.is_run_js_callable === true,
          helper: helperNames.indexOf(row.name) !== -1
        };
      });
      return {
        helpers: helperNames,
        persisted_globals: persistedGlobals,
        tools: normalized,
        top_level: normalized.filter(function(tool) { return tool.top_level; }).map(function(tool) { return tool.name; }),
        run_js_callable: normalized.filter(function(tool) { return tool.run_js_callable; }).map(function(tool) { return tool.name; }),
        note: 'Use helper methods for common run_js workflows; use tools.dispatch(name, args) for run_js-callable host tools without a helper.'
      };
    },

    persist_function(args = {}) {
      requireObject('persist_function', args);
      requireString('persist_function', args, 'name');
      requireString('persist_function', args, 'code');
      if (args.description !== undefined && typeof args.description !== 'string') {
        throw new TypeError('persist_function description must be a string');
      }
      if (args.test_args !== undefined && !Array.isArray(args.test_args)) {
        throw new TypeError('persist_function test_args must be an array');
      }
      return call('persist_function', args);
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

    write_file(key) {
      if (arguments.length !== 1) {
        throw new TypeError('tools.write_file inside run_js requires exactly one input key string');
      }
      const args = requireInputPayload('write_file', key);
      requireString('write_file', args, 'path');
      requireString('write_file', args, 'content');
      return call('write_file', args);
    },

    edit_tool(key) {
      if (arguments.length !== 1) {
        throw new TypeError('tools.edit_tool inside run_js requires exactly one input key string');
      }
      const args = requireInputPayload('edit_tool', key);
      requireString('edit_tool', args, 'path');
      if (!Array.isArray(args.edits)) throw new Error('INVALID_ARGUMENT: edit_tool.edits must be an array');
      return call('edit_tool', args);
    },

    execute_bash(args = {}) {
      requireObject('execute_bash', args);
      requireString('execute_bash', args, 'cwd');
      requireString('execute_bash', args, 'command');
      requireBoolean('execute_bash', args, 'allow_nonzero_exit');
      return call('execute_bash', args);
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

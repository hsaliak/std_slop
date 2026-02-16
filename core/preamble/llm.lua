local function _format_prompt(args)
  if type(args) == "string" then
    return args
  end
  
  if type(args) ~= "table" then
    error("llm_query: argument must be a string or a table")
  end

  local prompt = args.query or args.prompt
  local context = args.context or args.data
  
  if not prompt or not context then
    error("llm_query: Both 'query' (or 'prompt') and 'context' (or 'data') fields are required in table mode. " ..
          "Received: " .. (prompt and "query OK" or "query MISSING") .. ", " .. 
          (context and "context OK" or "context MISSING"))
  end
  
  -- If context is a table, join it
  if type(context) == "table" then
    context = table.concat(context, "\n\n")
  end
  
  return string.format("### INSTRUCTION ###\n%s\n\n### CONTEXT ###\n%s", prompt, context)
end

function llm_query(query)
  if not query or query == "" then error("llm_query requires a query string") end
  local success, result = call_tool(native.llm_query, {query = query})
  if not success then error("llm_query failed: " .. result) end
  return result
end

function llm_query_async(query)
  if not query or query == "" then error("llm_query_async requires a query string") end
  return tools.dispatch_async("llm_query", {query = query})
end

--- Queries the LLM with structured input.
--- @param args table|string If table: { query = "string", context = "string" | {"string", ...} }
function tools.llm_query(args)
  local formatted = _format_prompt(args)
  return llm_query(formatted)
end

--- Queries the LLM with structured input asynchronously.
--- @param args table|string If table: { query = "string", context = "string" | {"string", ...} }
function tools.llm_query_async(args)
  local formatted = _format_prompt(args)
  return llm_query_async(formatted)
end

function get_tool_manifest()
  local m = { tools = {} }
  for name, _ in pairs(tools) do
    table.insert(m.tools, name)
  end
  return m
end
